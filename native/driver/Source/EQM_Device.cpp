//
//  EQM_Device.cpp
//  EQMDriver
//
//  Copyright © 2017 Andrew Tonner
//  Portions copyright (C) 2013 Apple Inc. All Rights Reserved.
//
//  Based largely on SA_Device.cpp from Apple's SimpleAudioDriver Plug-In sample code. Also uses a few sections from Apple's
//  NullAudio.c sample code (found in the same sample project).
//  https://developer.apple.com/library/mac/samplecode/AudioDriverExamples
//

// Self Include
#include "EQM_Device.h"

// Local Includes
#include "EQM_PlugIn.h"
#include "EQM_XPCHelper.h"
#include "EQM_Utils.h"

// PublicUtility Includes
#include "CADispatchQueue.h"
#include "CAException.h"
#include "CACFArray.h"
#include "CACFString.h"
#include "CADebugMacros.h"
#include "CAHostTimeBase.h"

// STL Includes
#include <stdexcept>

// System Includes
#include <mach/mach_time.h>
#include <CoreAudio/AudioHardwareBase.h>


#pragma mark Construction/Destruction

pthread_once_t				EQM_Device::sStaticInitializer = PTHREAD_ONCE_INIT;
EQM_Device*					EQM_Device::sInstance = nullptr;
EQM_Device*					EQM_Device::sUISoundsInstance = nullptr;

EQM_Device& EQM_Device::GetInstance()
{
  pthread_once(&sStaticInitializer, StaticInitializer);
  return *sInstance;
}

EQM_Device&	EQM_Device::GetUISoundsInstance()
{
  pthread_once(&sStaticInitializer, StaticInitializer);
  return *sUISoundsInstance;
}

void	EQM_Device::StaticInitializer()
{
  try
  {
    // The main instance, usually referred to in the code as "EQMDevice". This is the device
    // that appears in System Preferences as "Background Music".
    sInstance = new EQM_Device(kObjectID_Device,
                               CFSTR(kDeviceName),
                               CFSTR(kEQMDeviceUID),
                               CFSTR(kEQMDeviceModelUID),
                               kObjectID_Stream_Input,
                               kObjectID_Stream_Output,
                               kObjectID_Volume_Output_Master,
                               kObjectID_Mute_Output_Master);
    sInstance->Activate();
    
    if (
        !EQM_Utils::process_at_path_running("/Applications/eqMac.app/Contents/MacOS/eqMac")
        && !EQM_Utils::process_at_path_running("/Applications/Xcode.app/Contents/MacOS/Xcode")
    ) {
      // Activate the Driver only if eqMac or Xcode are running from /Application folder
      sInstance->Deactivate();
    } else {
    }
        
    // The instance for system (UI) sounds.
    sUISoundsInstance = new EQM_Device(kObjectID_Device_UI_Sounds,
                                       CFSTR(kDeviceName_UISounds),
                                       CFSTR(kEQMDeviceUID_UISounds),
                                       CFSTR(kEQMDeviceModelUID_UISounds),
                                       kObjectID_Stream_Input_UI_Sounds,
                                       kObjectID_Stream_Output_UI_Sounds,
                                       kObjectID_Volume_Output_Master_UI_Sounds,
                                       kAudioObjectUnknown);  // No mute control.
    
    // Set up the UI sounds device's volume control.
    EQM_VolumeControl& theUISoundsVolumeControl = sUISoundsInstance->mVolumeControl;
    // Default to full volume.
    theUISoundsVolumeControl.SetVolumeScalar(1.0f);
    // Make the volume curve a bit steeper than the default.
    theUISoundsVolumeControl.GetVolumeCurve().SetTransferFunction(CAVolumeCurve::kPow4Over1Curve);
    // Apply the volume to the device's output stream. The main instance of EQM_Device doesn't
    // apply volume to its audio because EQMApp changes the real output device's volume directly
    // instead.
    theUISoundsVolumeControl.SetWillApplyVolumeToAudio(true);
    
    //        sUISoundsInstance->Activate();
  }
  catch(...)
  {
    DebugMsg("EQM_Device::StaticInitializer: failed to create the devices");
    
    delete sInstance;
    sInstance = nullptr;
    
    delete sUISoundsInstance;
    sUISoundsInstance = nullptr;
  }
}

EQM_Device::EQM_Device(AudioObjectID inObjectID,
                       const CFStringRef __nonnull inDeviceName,
                       const CFStringRef __nonnull inDeviceUID,
                       const CFStringRef __nonnull inDeviceModelUID,
                       AudioObjectID inInputStreamID,
                       AudioObjectID inOutputStreamID,
                       AudioObjectID inOutputVolumeControlID,
                       AudioObjectID inOutputMuteControlID)
:
EQM_AbstractDevice(inObjectID, kAudioObjectPlugInObject),
mStateMutex("Device State"),
mIOMutex("Device IO"),
mDeviceName(inDeviceName),
mDeviceUID(inDeviceUID),
mDeviceModelUID(inDeviceModelUID),
mWrappedAudioEngine(nullptr),
mClients(inObjectID, &mTaskQueue),
mInputStream(inInputStreamID, inObjectID, false, kSampleRateDefault),
mOutputStream(inOutputStreamID, inObjectID, false, kSampleRateDefault),
mAudibleState(),
mVolumeControl(inOutputVolumeControlID, GetObjectID(), kAudioObjectPropertyScopeOutput),
mMuteControl(inOutputMuteControlID, GetObjectID(), kAudioObjectPropertyScopeOutput)
{
  // Initialises the loopback clock with the default sample rate and, if there is one, sets the wrapped device to the same sample rate
  SetSampleRate(kSampleRateDefault);
  SetSafetyOffset(kSafetyOffsetDefault);
  SetLatency(kLatencyDefault);
}

EQM_Device::~EQM_Device()
{
}

void	EQM_Device::Activate()
{
  CAMutex::Locker theStateLocker(mStateMutex);
  
  //	Open the connection to the driver and initialize things.
  //_HW_Open();
  
  mInputStream.Activate();
  mOutputStream.Activate();
  
  if(mVolumeControl.GetObjectID() != kAudioObjectUnknown)
  {
    mVolumeControl.Activate();
  }
  
  if(mMuteControl.GetObjectID() != kAudioObjectUnknown)
  {
    mMuteControl.Activate();
  }
  
  //	Call the super-class, which just marks the object as active
  EQM_AbstractDevice::Activate();
}

void	EQM_Device::Deactivate()
{
  //	When this method is called, the object is basically dead, but we still need to be thread
  //	safe. In this case, we also need to be safe vs. any IO threads, so we need to take both
  //	locks.
  CAMutex::Locker theStateLocker(mStateMutex);
  CAMutex::Locker theIOLocker(mIOMutex);
  
  // Mark the device's sub-objects inactive.
  mInputStream.Deactivate();
  mOutputStream.Deactivate();
  mVolumeControl.Deactivate();
  mMuteControl.Deactivate();
  
  //	mark the object inactive by calling the super-class
  EQM_AbstractDevice::Deactivate();
  
  //	close the connection to the driver
  //_HW_Close();
}

void    EQM_Device::InitLoopback()
{
  // Calculate the number of host clock ticks per frame for our loopback clock.
  mLoopbackTime.hostTicksPerFrame = CAHostTimeBase::GetFrequency() / mLoopbackSampleRate;
  
  //  Zero-out the loopback buffer
  //  2 channels * 32-bit float = bytes in each frame
  memset(mLoopbackRingBuffer, 0, sizeof(Float32) * 2 * kLoopbackRingBufferFrameSize);
}

#pragma mark Property Operations

bool	EQM_Device::HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
  //	This object owns several API-level objects. So the first thing to do is to figure out
  //	which object this request is really for. Note that mObjectID is an invariant as this
  //	driver's structure does not change dynamically. It will always have the parts it has.
  bool theAnswer = false;
  
  if(inObjectID == mObjectID)
  {
    theAnswer = Device_HasProperty(inObjectID, inClientPID, inAddress);
  }
  else
  {
    theAnswer = GetOwnedObjectByID(inObjectID).HasProperty(inObjectID, inClientPID, inAddress);
  }
  
  return theAnswer;
}

bool	EQM_Device::IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
  bool theAnswer = false;
  
  if(inObjectID == mObjectID)
  {
    theAnswer = Device_IsPropertySettable(inObjectID, inClientPID, inAddress);
  }
  else
  {
    theAnswer = GetOwnedObjectByID(inObjectID).IsPropertySettable(inObjectID, inClientPID, inAddress);
  }
  
  return theAnswer;
}

UInt32	EQM_Device::GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
  UInt32 theAnswer = 0;
  
  if(inObjectID == mObjectID)
  {
    theAnswer = Device_GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
  }
  else
  {
    theAnswer = GetOwnedObjectByID(inObjectID).GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
  }
  
  return theAnswer;
}

void	EQM_Device::GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
  ThrowIfNULL(outData, std::runtime_error("!outData"), "EQM_Device::GetPropertyData: !outData");
  
  if(inObjectID == mObjectID)
  {
    Device_GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
  }
  else
  {
    GetOwnedObjectByID(inObjectID).GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
  }
}

void	EQM_Device::SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
  ThrowIfNULL(inData, std::runtime_error("no data"), "EQM_Device::SetPropertyData: no data");
  
  if(inObjectID == mObjectID)
  {
    Device_SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
  }
  else
  {
    GetOwnedObjectByID(inObjectID).SetPropertyData(inObjectID,
                                                   inClientPID,
                                                   inAddress,
                                                   inQualifierDataSize,
                                                   inQualifierData,
                                                   inDataSize,
                                                   inData);
    if(IsStreamID(inObjectID))
    {
      // When one of the stream's sample rate changes, set the new sample rate for both
      // streams and the device. The streams check the new format before this point but don't
      // change until the device tells them to, as it has to get the host to pause IO first.
      if(inAddress.mSelector == kAudioStreamPropertyVirtualFormat ||
         inAddress.mSelector == kAudioStreamPropertyPhysicalFormat)
      {
        const AudioStreamBasicDescription* theNewFormat =
        reinterpret_cast<const AudioStreamBasicDescription*>(inData);
        RequestSampleRate(theNewFormat->mSampleRate);
      }
    }
  }
}

#pragma mark Device Property Operations

bool	EQM_Device::Device_HasProperty(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
  //	For each object, this driver implements all the required properties plus a few extras that
  //	are useful but not required. There is more detailed commentary about each property in the
  //	Device_GetPropertyData() method.
  
  bool theAnswer = false;
  switch(inAddress.mSelector)
  {
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertyIcon:
    case kAudioObjectPropertyCustomPropertyInfoList:
    case kAudioDeviceCustomPropertyDeviceAudibleState:
    case kAudioDeviceCustomPropertyMusicPlayerProcessID:
    case kAudioDeviceCustomPropertyMusicPlayerBundleID:
    case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp:
    case kAudioDeviceCustomPropertyAppVolumes:
    case kAudioDeviceCustomPropertyEnabledOutputControls:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDeviceCustomPropertyLatency:
    case kAudioDeviceCustomPropertySafetyOffset:
      theAnswer = true;
      break;
      
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
      theAnswer = (inAddress.mScope == kAudioObjectPropertyScopeInput) || (inAddress.mScope == kAudioObjectPropertyScopeOutput);
      break;
    default:
      theAnswer = EQM_AbstractDevice::HasProperty(inObjectID, inClientPID, inAddress);
      break;
  };
  return theAnswer;
}

bool	EQM_Device::Device_IsPropertySettable(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress) const
{
  //	For each object, this driver implements all the required properties plus a few extras that
  //	are useful but not required. There is more detailed commentary about each property in the
  //	Device_GetPropertyData() method.
  bool theAnswer = false;
  switch(inAddress.mSelector)
  {
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyIcon:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioObjectPropertyCustomPropertyInfoList:
    case kAudioDeviceCustomPropertyDeviceAudibleState:
    case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp:
      theAnswer = false;
      break;
      
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDeviceCustomPropertyMusicPlayerProcessID:
    case kAudioDeviceCustomPropertyMusicPlayerBundleID:
    case kAudioDeviceCustomPropertyAppVolumes:
    case kAudioDeviceCustomPropertyEnabledOutputControls:
    case kAudioDeviceCustomPropertyLatency:
    case kAudioDeviceCustomPropertySafetyOffset:
      theAnswer = true;
      break;
      
    default:
      theAnswer = EQM_AbstractDevice::IsPropertySettable(inObjectID, inClientPID, inAddress);
      break;
  };
  return theAnswer;
}

UInt32	EQM_Device::Device_GetPropertyDataSize(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData) const
{
  //	For each object, this driver implements all the required properties plus a few extras that
  //	are useful but not required. There is more detailed commentary about each property in the
  //	Device_GetPropertyData() method.
  
  UInt32 theAnswer = 0;
  
  switch(inAddress.mSelector)
  {
    case kAudioObjectPropertyOwnedObjects:
    {
      switch(inAddress.mScope)
      {
        case kAudioObjectPropertyScopeGlobal:
          theAnswer = GetNumberOfSubObjects() * sizeof(AudioObjectID);
          break;
          
        case kAudioObjectPropertyScopeInput:
          theAnswer = kNumberOfInputSubObjects * sizeof(AudioObjectID);
          break;
          
        case kAudioObjectPropertyScopeOutput:
          theAnswer = kNumberOfOutputStreams * sizeof(AudioObjectID);
          theAnswer += GetNumberOfOutputControls() * sizeof(AudioObjectID);
          break;
          
        default:
          break;
      };
    }
      break;
      
    case kAudioDevicePropertyStreams:
    {
      switch(inAddress.mScope)
      {
        case kAudioObjectPropertyScopeGlobal:
          theAnswer = kNumberOfStreams * sizeof(AudioObjectID);
          break;
          
        case kAudioObjectPropertyScopeInput:
          theAnswer = kNumberOfInputStreams * sizeof(AudioObjectID);
          break;
          
        case kAudioObjectPropertyScopeOutput:
          theAnswer = kNumberOfOutputStreams * sizeof(AudioObjectID);
          break;
          
        default:
          break;
      };
    }
      break;
      
    case kAudioObjectPropertyControlList:
      theAnswer = GetNumberOfOutputControls() * sizeof(AudioObjectID);
      break;
      
    case kAudioDevicePropertyAvailableNominalSampleRates:
      theAnswer = 1 * sizeof(AudioValueRange);
      break;
      
    case kAudioDevicePropertyPreferredChannelsForStereo:
      theAnswer = 2 * sizeof(UInt32);
      break;
      
    case kAudioDevicePropertyPreferredChannelLayout:
      theAnswer = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
      break;
      
    case kAudioDevicePropertyIcon:
      theAnswer = sizeof(CFURLRef);
      break;
      
    case kAudioObjectPropertyCustomPropertyInfoList:
      theAnswer = sizeof(AudioServerPlugInCustomPropertyInfo) * 8;
      break;
      
    case kAudioDeviceCustomPropertyDeviceAudibleState:
      theAnswer = sizeof(CFNumberRef);
      break;
      
    case kAudioDeviceCustomPropertyMusicPlayerProcessID:
      theAnswer = sizeof(CFPropertyListRef);
      break;
      
    case kAudioDeviceCustomPropertyMusicPlayerBundleID:
      theAnswer = sizeof(CFStringRef);
      break;
      
    case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp:
      theAnswer = sizeof(CFBooleanRef);
      break;
      
    case kAudioDeviceCustomPropertyAppVolumes:
      theAnswer = sizeof(CFPropertyListRef);
      break;
      
    case kAudioDeviceCustomPropertyEnabledOutputControls:
      theAnswer = sizeof(CFArrayRef);
      break;
    case kAudioDeviceCustomPropertyLatency:
      theAnswer = sizeof(CFNumberRef);
      break;
      
    case kAudioDeviceCustomPropertySafetyOffset:
      theAnswer = sizeof(CFNumberRef);
      break;
      
    default:
      theAnswer = EQM_AbstractDevice::GetPropertyDataSize(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData);
      break;
  };
  
  return theAnswer;
}

void	EQM_Device::Device_GetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32& outDataSize, void* outData) const
{
  //	For each object, this driver implements all the required properties plus a few extras that
  //	are useful but not required.
  //	Also, since most of the data that will get returned is static, there are few instances where
  //	it is necessary to lock the state mutex.
  
  UInt32 theNumberItemsToFetch;
  UInt32 theItemIndex;
  
  switch(inAddress.mSelector)
  {
    case kAudioObjectPropertyName:
      //	This is the human readable name of the device. Note that in this case we return a
      //	value that is a key into the localizable strings in this bundle. This allows us to
      //	return a localized name for the device.
      ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyName for the device");
      *reinterpret_cast<CFStringRef*>(outData) = mDeviceName;
      outDataSize = sizeof(CFStringRef);
      break;
      
    case kAudioObjectPropertyManufacturer:
      //	This is the human readable name of the maker of the plug-in. Note that in this case
      //	we return a value that is a key into the localizable strings in this bundle. This
      //	allows us to return a localized name for the manufacturer.
      ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device");
      *reinterpret_cast<CFStringRef*>(outData) = CFSTR(kDeviceManufacturerName);
      outDataSize = sizeof(CFStringRef);
      break;
      
    case kAudioObjectPropertyOwnedObjects:
      //	Calculate the number of items that have been requested. Note that this
      //	number is allowed to be smaller than the actual size of the list. In such
      //	case, only that number of items will be returned
      theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
      
      //	The device owns its streams and controls. Note that what is returned here
      //	depends on the scope requested.
      switch(inAddress.mScope)
    {
      case kAudioObjectPropertyScopeGlobal:
        //	global scope means return all objects
      {
        CAMutex::Locker theStateLocker(mStateMutex);
        
        if(theNumberItemsToFetch > GetNumberOfSubObjects())
        {
          theNumberItemsToFetch = GetNumberOfSubObjects();
        }
        
        //	fill out the list with as many objects as requested, which is everything
        if(theNumberItemsToFetch > 0)
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mInputStream.GetObjectID();
        }
        
        if(theNumberItemsToFetch > 1)
        {
          reinterpret_cast<AudioObjectID*>(outData)[1] = mOutputStream.GetObjectID();
        }
        
        // If at least one of the controls is enabled, and there's room, return one.
        if(theNumberItemsToFetch > 2)
        {
          if(mVolumeControl.IsActive())
          {
            reinterpret_cast<AudioObjectID*>(outData)[2] = mVolumeControl.GetObjectID();
          }
          else if(mMuteControl.IsActive())
          {
            reinterpret_cast<AudioObjectID*>(outData)[2] = mMuteControl.GetObjectID();
          }
        }
        
        // If both controls are enabled, and there's room, return the mute control as well.
        if(theNumberItemsToFetch > 3 && mVolumeControl.IsActive() && mMuteControl.IsActive())
        {
          reinterpret_cast<AudioObjectID*>(outData)[3] = mMuteControl.GetObjectID();
        }
      }
        break;
        
      case kAudioObjectPropertyScopeInput:
        //	input scope means just the objects on the input side
        if(theNumberItemsToFetch > kNumberOfInputSubObjects)
        {
          theNumberItemsToFetch = kNumberOfInputSubObjects;
        }
        
        //	fill out the list with the right objects
        if(theNumberItemsToFetch > 0)
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mInputStream.GetObjectID();
        }
        break;
        
      case kAudioObjectPropertyScopeOutput:
        //	output scope means just the objects on the output side
      {
        CAMutex::Locker theStateLocker(mStateMutex);
        
        if(theNumberItemsToFetch > GetNumberOfOutputControls())
        {
          theNumberItemsToFetch = GetNumberOfOutputControls();
        }
        
        //	fill out the list with the right objects
        if(theNumberItemsToFetch > 0)
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mOutputStream.GetObjectID();
        }
        
        // If at least one of the controls is enabled, and there's room, return one.
        if(theNumberItemsToFetch > 1)
        {
          if(mVolumeControl.IsActive())
          {
            reinterpret_cast<AudioObjectID*>(outData)[1] = mVolumeControl.GetObjectID();
          }
          else if(mMuteControl.IsActive())
          {
            reinterpret_cast<AudioObjectID*>(outData)[1] = mMuteControl.GetObjectID();
          }
        }
        
        // If both controls are enabled, and there's room, return the mute control as well.
        if(theNumberItemsToFetch > 2 && mVolumeControl.IsActive() && mMuteControl.IsActive())
        {
          reinterpret_cast<AudioObjectID*>(outData)[2] = mMuteControl.GetObjectID();
        }
      }
        break;
    };
      
      //	report how much we wrote
      outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
      break;
      
    case kAudioDevicePropertyDeviceUID:
      //	This is a CFString that is a persistent token that can identify the same
      //	audio device across boot sessions. Note that two instances of the same
      //	device must have different values for this property.
      ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device");
      *reinterpret_cast<CFStringRef*>(outData) = mDeviceUID;
      outDataSize = sizeof(CFStringRef);
      break;
      
    case kAudioDevicePropertyModelUID:
      //	This is a CFString that is a persistent token that can identify audio
      //	devices that are the same kind of device. Note that two instances of the
      //	save device must have the same value for this property.
      ThrowIf(inDataSize < sizeof(AudioObjectID), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device");
      *reinterpret_cast<CFStringRef*>(outData) = mDeviceModelUID;
      outDataSize = sizeof(CFStringRef);
      break;
      
    case kAudioDevicePropertyDeviceIsRunning:
      //	This property returns whether or not IO is running for the device.
      ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device");
      *reinterpret_cast<UInt32*>(outData) = mClients.ClientsRunningIO() ? 1 : 0;
      outDataSize = sizeof(UInt32);
      break;
      
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
      // See EQM_AbstractDevice::GetPropertyData.
      //
      // We don't allow the UI Sounds instance of EQM_Device to be set as the default device
      // so that it doesn't appear in the list of devices, which would just be confusing to
      // users. (And it wouldn't make sense to set it as the default device anyway.)
      //
      // Instead, EQMApp sets the UI Sounds device as the "system default" (see
      // kAudioDevicePropertyDeviceCanBeDefaultSystemDevice) so apps will use it for
      // UI-related sounds.
      ThrowIf(inDataSize < sizeof(UInt32),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::GetPropertyData: not enough space for the return value of "
              "kAudioDevicePropertyDeviceCanBeDefaultDevice for the device");
      // TODO: Add a field for this and set it in EQM_Device::StaticInitializer so we don't
      //       have to handle a specific instance differently here.
      *reinterpret_cast<UInt32*>(outData) = (GetObjectID() == kObjectID_Device_UI_Sounds ? 0 : 1);
      outDataSize = sizeof(UInt32);
      break;
      
    case kAudioDevicePropertyStreams:
      //	Calculate the number of items that have been requested. Note that this
      //	number is allowed to be smaller than the actual size of the list. In such
      //	case, only that number of items will be returned
      theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
      
      //	Note that what is returned here depends on the scope requested.
      switch(inAddress.mScope)
    {
      case kAudioObjectPropertyScopeGlobal:
        //	global scope means return all streams
        if(theNumberItemsToFetch > kNumberOfStreams)
        {
          theNumberItemsToFetch = kNumberOfStreams;
        }
        
        //	fill out the list with as many objects as requested
        if(theNumberItemsToFetch > 0)
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mInputStream.GetObjectID();
        }
        if(theNumberItemsToFetch > 1)
        {
          reinterpret_cast<AudioObjectID*>(outData)[1] = mOutputStream.GetObjectID();
        }
        break;
        
      case kAudioObjectPropertyScopeInput:
        //	input scope means just the objects on the input side
        if(theNumberItemsToFetch > kNumberOfInputStreams)
        {
          theNumberItemsToFetch = kNumberOfInputStreams;
        }
        
        //	fill out the list with as many objects as requested
        if(theNumberItemsToFetch > 0)
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mInputStream.GetObjectID();
        }
        break;
        
      case kAudioObjectPropertyScopeOutput:
        //	output scope means just the objects on the output side
        if(theNumberItemsToFetch > kNumberOfOutputStreams)
        {
          theNumberItemsToFetch = kNumberOfOutputStreams;
        }
        
        //	fill out the list with as many objects as requested
        if(theNumberItemsToFetch > 0)
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mOutputStream.GetObjectID();
        }
        break;
    };
      
      //	report how much we wrote
      outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID);
      break;
      
    case kAudioObjectPropertyControlList:
    {
      //	Calculate the number of items that have been requested. Note that this
      //	number is allowed to be smaller than the actual size of the list, in which
      //	case only that many items will be returned.
      theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID);
      if(theNumberItemsToFetch > 2)
      {
        theNumberItemsToFetch = 2;
      }
      
      UInt32 theNumberOfItemsFetched = 0;
      
      CAMutex::Locker theStateLocker(mStateMutex);
      
      //	fill out the list with as many objects as requested
      if(theNumberItemsToFetch > 0)
      {
        if(mVolumeControl.IsActive())
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mVolumeControl.GetObjectID();
          theNumberOfItemsFetched++;
        }
        else if(mMuteControl.IsActive())
        {
          reinterpret_cast<AudioObjectID*>(outData)[0] = mMuteControl.GetObjectID();
          theNumberOfItemsFetched++;
        }
      }
      
      if(theNumberItemsToFetch > 1 && mVolumeControl.IsActive() && mMuteControl.IsActive())
      {
        reinterpret_cast<AudioObjectID*>(outData)[1] = mMuteControl.GetObjectID();
        theNumberOfItemsFetched++;
      }
      
      //	report how much we wrote
      outDataSize = theNumberOfItemsFetched * sizeof(AudioObjectID);
    }
      break;
      
    case kAudioDevicePropertyLatency:
      //  This property returns the latency of the device.
      ThrowIf(inDataSize < sizeof(UInt32),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyLatency for the device");
      
      *reinterpret_cast<UInt32*>(outData) = GetLatency();
      outDataSize = sizeof(UInt32);
      break;
      
    case kAudioDevicePropertySafetyOffset:
      //  This property returns the safety offset of the device.
      ThrowIf(inDataSize < sizeof(UInt32),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertySafetyOffset for the device");
      *reinterpret_cast<UInt32*>(outData) = GetSafetyOffset();
      outDataSize = sizeof(UInt32);
      break;
      
    case kAudioDevicePropertyNominalSampleRate:
      //	This property returns the nominal sample rate of the device.
      ThrowIf(inDataSize < sizeof(Float64),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device");
      
      *reinterpret_cast<Float64*>(outData) = GetSampleRate();
      outDataSize = sizeof(Float64);
      break;
      
    case kAudioDevicePropertyAvailableNominalSampleRates:
      //	This returns all nominal sample rates the device supports as an array of
      //	AudioValueRangeStructs. Note that for discrete sampler rates, the range
      //	will have the minimum value equal to the maximum value.
      //
      //  EQMDevice supports any sample rate so it can be set to match the output
      //  device when in loopback mode.
      
      //	Calculate the number of items that have been requested. Note that this
      //	number is allowed to be smaller than the actual size of the list. In such
      //	case, only that number of items will be returned
      theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange);
      
      //	clamp it to the number of items we have
      if(theNumberItemsToFetch > 1)
      {
        theNumberItemsToFetch = 1;
      }
      
      //	fill out the return array
      if(theNumberItemsToFetch > 0)
      {
        // 0 would cause divide-by-zero errors in other EQM_Device functions (and
        // wouldn't make sense anyway).
        ((AudioValueRange*)outData)[0].mMinimum = 1.0;
        // Just in case DBL_MAX would cause problems in a client for some reason,
        // use an arbitrary very large number instead. (It wouldn't make sense to
        // actually set the sample rate this high, but I don't know what a
        // reasonable maximum would be.)
        ((AudioValueRange*)outData)[0].mMaximum = 1000000000.0;
      }
      
      //	report how much we wrote
      outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange);
      break;
      
    case kAudioDevicePropertyPreferredChannelsForStereo:
      //	This property returns which two channels to use as left/right for stereo
      //	data by default. Note that the channel numbers are 1-based.
      ThrowIf(inDataSize < (2 * sizeof(UInt32)), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelsForStereo for the device");
      ((UInt32*)outData)[0] = 1;
      ((UInt32*)outData)[1] = 2;
      outDataSize = 2 * sizeof(UInt32);
      break;
      
    case kAudioDevicePropertyPreferredChannelLayout:
      //	This property returns the default AudioChannelLayout to use for the device
      //	by default. For this device, we return a stereo ACL.
    {
      UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (2 * sizeof(AudioChannelDescription));
      ThrowIf(inDataSize < theACLSize, CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device");
      ((AudioChannelLayout*)outData)->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
      ((AudioChannelLayout*)outData)->mChannelBitmap = 0;
      ((AudioChannelLayout*)outData)->mNumberChannelDescriptions = 2;
      for(theItemIndex = 0; theItemIndex < 2; ++theItemIndex)
      {
        ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelLabel = kAudioChannelLabel_Left + theItemIndex;
        ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelFlags = 0;
        ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[0] = 0;
        ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[1] = 0;
        ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[2] = 0;
      }
      outDataSize = theACLSize;
    }
      break;
      
    case kAudioDevicePropertyZeroTimeStampPeriod:
      //	This property returns how many frames the HAL should expect to see between
      //	successive sample times in the zero time stamps this device provides.
      ThrowIf(inDataSize < sizeof(UInt32), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device");
      *reinterpret_cast<UInt32*>(outData) = kLoopbackRingBufferFrameSize;
      outDataSize = sizeof(UInt32);
      break;
      
    case kAudioDevicePropertyIcon:
    {
      // This property is a CFURL that points to the device's icon in the plugin's resource bundle
      ThrowIf(inDataSize < sizeof(CFURLRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDevicePropertyIcon for the device");
      
      CFBundleRef theBundle = CFBundleGetBundleWithIdentifier(EQM_PlugIn::GetInstance().GetBundleID());
      ThrowIf(theBundle == NULL, CAException(kAudioHardwareUnspecifiedError), "EQM_Device::Device_GetPropertyData: could not get the plugin bundle for kAudioDevicePropertyIcon");
      
      CFURLRef theURL = CFBundleCopyResourceURL(theBundle, CFSTR("icon.icns"), NULL, NULL);
      ThrowIf(theURL == NULL, CAException(kAudioHardwareUnspecifiedError), "EQM_Device::Device_GetPropertyData: could not get the URL for kAudioDevicePropertyIcon");
      
      *reinterpret_cast<CFURLRef*>(outData) = theURL;
      outDataSize = sizeof(CFURLRef);
    }
      break;
      
    case kAudioObjectPropertyCustomPropertyInfoList:
      theNumberItemsToFetch = inDataSize / sizeof(AudioServerPlugInCustomPropertyInfo);
      
      //	clamp it to the number of items we have
      if(theNumberItemsToFetch > 8)
      {
        theNumberItemsToFetch = 8;
      }
      
      if(theNumberItemsToFetch > 0)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[0].mSelector = kAudioDeviceCustomPropertyAppVolumes;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[0].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[0].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      if(theNumberItemsToFetch > 1)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[1].mSelector = kAudioDeviceCustomPropertyMusicPlayerProcessID;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[1].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[1].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      if(theNumberItemsToFetch > 2)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[2].mSelector = kAudioDeviceCustomPropertyMusicPlayerBundleID;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[2].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[2].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      if(theNumberItemsToFetch > 3)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[3].mSelector = kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[3].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[3].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      if(theNumberItemsToFetch > 4)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[4].mSelector = kAudioDeviceCustomPropertyDeviceAudibleState;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[4].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[4].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      if(theNumberItemsToFetch > 5)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mSelector = kAudioDeviceCustomPropertyEnabledOutputControls;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      if(theNumberItemsToFetch > 6)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mSelector = kAudioDeviceCustomPropertyLatency;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[5].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      
      if(theNumberItemsToFetch > 7)
      {
        ((AudioServerPlugInCustomPropertyInfo*)outData)[6].mSelector = kAudioDeviceCustomPropertySafetyOffset;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[6].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
        ((AudioServerPlugInCustomPropertyInfo*)outData)[6].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
      }
      
      outDataSize = theNumberItemsToFetch * sizeof(AudioServerPlugInCustomPropertyInfo);
      break;
      
    case kAudioDeviceCustomPropertyDeviceAudibleState:
    {
      ThrowIf(inDataSize < sizeof(CFNumberRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyDeviceAudibleState for the device");
      
      // The audible state is read without locking to avoid priority inversions on the IO threads.
      EQMDeviceAudibleState theAudibleState = mAudibleState.GetState();
      *reinterpret_cast<CFNumberRef*>(outData) =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &theAudibleState);
      outDataSize = sizeof(CFNumberRef);
    }
      break;
      
    case kAudioDeviceCustomPropertyMusicPlayerProcessID:
    {
      ThrowIf(inDataSize < sizeof(CFNumberRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyMusicPlayerProcessID for the device");
      CAMutex::Locker theStateLocker(mStateMutex);
      pid_t pid = mClients.GetMusicPlayerProcessIDProperty();
      *reinterpret_cast<CFNumberRef*>(outData) = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
      outDataSize = sizeof(CFNumberRef);
    }
      break;
      
    case kAudioDeviceCustomPropertyMusicPlayerBundleID:
    {
      ThrowIf(inDataSize < sizeof(CFStringRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyMusicPlayerBundleID for the device");
      CAMutex::Locker theStateLocker(mStateMutex);
      *reinterpret_cast<CFStringRef*>(outData) = mClients.CopyMusicPlayerBundleIDProperty();
      outDataSize = sizeof(CFStringRef);
    }
      break;
      
    case kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp:
      ThrowIf(inDataSize < sizeof(CFBooleanRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp for the device");
      *reinterpret_cast<CFBooleanRef*>(outData) = mClients.ClientsOtherThanEQMAppRunningIO() ? kCFBooleanTrue : kCFBooleanFalse;
      outDataSize = sizeof(CFBooleanRef);
      break;
      
    case kAudioDeviceCustomPropertyAppVolumes:
    {
      ThrowIf(inDataSize < sizeof(CFArrayRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyAppVolumes for the device");
      CAMutex::Locker theStateLocker(mStateMutex);
      *reinterpret_cast<CFArrayRef*>(outData) = mClients.CopyClientRelativeVolumesAsAppVolumes().GetCFArray();
      outDataSize = sizeof(CFArrayRef);
    }
      break;
      
    case kAudioDeviceCustomPropertyEnabledOutputControls:
    {
      ThrowIf(inDataSize < sizeof(CFArrayRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_GetPropertyData: not enough space for the return value of kAudioDeviceCustomPropertyEnabledOutputControls for the device");
      CACFArray theEnabledControls(2, true);
      
      {
        CAMutex::Locker theStateLocker(mStateMutex);
        theEnabledControls.AppendCFType(mVolumeControl.IsActive() ? kCFBooleanTrue : kCFBooleanFalse);
        theEnabledControls.AppendCFType(mMuteControl.IsActive() ? kCFBooleanTrue : kCFBooleanFalse);
      }
      
      *reinterpret_cast<CFArrayRef*>(outData) = theEnabledControls.CopyCFArray();
      outDataSize = sizeof(CFArrayRef);
    }
      break;
      
    default:
      EQM_AbstractDevice::GetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
      break;
  };
}

void	EQM_Device::Device_SetPropertyData(AudioObjectID inObjectID, pid_t inClientPID, const AudioObjectPropertyAddress& inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
  
  switch(inAddress.mSelector)
  {
    case kAudioDeviceCustomPropertyLatency:
    {
      ThrowIf(inDataSize < sizeof(CFNumberRef),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyLatency");
      CFNumberRef latencyRef = *reinterpret_cast<const CFNumberRef*>(inData);
      uint32_t latency = 0;
      CFNumberGetValue(latencyRef, kCFNumberSInt32Type, &latency);
      ThrowIf(latency < 0,
              CAException(kAudioHardwareIllegalOperationError),
              "EQM_Device::Device_SetPropertyData: kAudioDeviceCustomPropertyLatency cannot be lower than 0");
      SetLatency(latency);
      break;
    }
      
    case kAudioDeviceCustomPropertySafetyOffset:
    {
      ThrowIf(inDataSize < sizeof(CFNumberRef),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertySafetyOffset");
      CFNumberRef safetyOffsetRef = *reinterpret_cast<const CFNumberRef*>(inData);
      uint32_t safetyOffset = 0;
      CFNumberGetValue(safetyOffsetRef, kCFNumberSInt32Type, &safetyOffset);
      ThrowIf(safetyOffset < 0,
              CAException(kAudioHardwareIllegalOperationError),
              "EQM_Device::Device_SetPropertyData: kAudioDeviceCustomPropertySafetyOffset cannot be lower than 0");
      SetSafetyOffset(safetyOffset);
      break;
    }
      
    case kAudioDevicePropertyNominalSampleRate: {
      ThrowIf(inDataSize < sizeof(Float64),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_SetPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate");
      RequestSampleRate(*reinterpret_cast<const Float64*>(inData));
      break;
      
    }
      
    case kAudioDeviceCustomPropertyMusicPlayerProcessID:
    {
      ThrowIf(inDataSize < sizeof(CFNumberRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyMusicPlayerProcessID");
      
      CFNumberRef pidRef = *reinterpret_cast<const CFNumberRef*>(inData);
      
      ThrowIf(pidRef == NULL, CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: null reference given for kAudioDeviceCustomPropertyMusicPlayerProcessID");
      ThrowIf(CFGetTypeID(pidRef) != CFNumberGetTypeID(), CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: CFType given for kAudioDeviceCustomPropertyMusicPlayerProcessID was not a CFNumber");
      
      // Get the pid out of the CFNumber we received
      // (Not using CACFNumber::GetSInt32 here because it would return 0 if CFNumberGetValue didn't write to our
      // pid variable, and we want that to be an error.)
      pid_t pid = INT_MIN;
      // CFNumberGetValue docs: "If the conversion is lossy, or the value is out of range, false is returned."
      Boolean success = CFNumberGetValue(pidRef, kCFNumberIntType, &pid);
      
      ThrowIf(!success, CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: probable error from CFNumberGetValue when reading pid for kAudioDeviceCustomPropertyMusicPlayerProcessID");
      
      CAMutex::Locker theStateLocker(mStateMutex);
      
      bool propertyWasChanged = false;
      
      try
      {
        propertyWasChanged = mClients.SetMusicPlayer(pid);
      }
      catch(EQM_InvalidClientPIDException)
      {
        Throw(CAException(kAudioHardwareIllegalOperationError));
      }
      
      if(propertyWasChanged)
      {
        // Send notification
        CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
          AudioObjectPropertyAddress theChangedProperties[] = { kEQMMusicPlayerProcessIDAddress, kEQMMusicPlayerBundleIDAddress };
          EQM_PlugIn::Host_PropertiesChanged(inObjectID, 2, theChangedProperties);
        });
      }
    }
      break;
      
    case kAudioDeviceCustomPropertyMusicPlayerBundleID:
    {
      ThrowIf(inDataSize < sizeof(CFStringRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyMusicPlayerBundleID");
      
      CFStringRef theBundleIDRef = *reinterpret_cast<const CFStringRef*>(inData);
      
      ThrowIfNULL(theBundleIDRef, CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: kAudioDeviceCustomPropertyMusicPlayerBundleID cannot be set to NULL");
      ThrowIf(CFGetTypeID(theBundleIDRef) != CFStringGetTypeID(), CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: CFType given for kAudioDeviceCustomPropertyMusicPlayerBundleID was not a CFString");
      
      CAMutex::Locker theStateLocker(mStateMutex);
      
      CFRetain(theBundleIDRef);
      CACFString bundleID(theBundleIDRef);
      
      bool propertyWasChanged = mClients.SetMusicPlayer(bundleID);
      
      if(propertyWasChanged)
      {
        // Send notification
        CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
          AudioObjectPropertyAddress theChangedProperties[] = { kEQMMusicPlayerBundleIDAddress, kEQMMusicPlayerProcessIDAddress };
          EQM_PlugIn::Host_PropertiesChanged(inObjectID, 2, theChangedProperties);
        });
      }
    }
      break;
      
    case kAudioDeviceCustomPropertyAppVolumes:
    {
      ThrowIf(inDataSize < sizeof(CFArrayRef), CAException(kAudioHardwareBadPropertySizeError), "EQM_Device::Device_SetPropertyData: wrong size for the data for kAudioDeviceCustomPropertyAppVolumes");
      
      CFArrayRef arrayRef = *reinterpret_cast<const CFArrayRef*>(inData);
      
      ThrowIfNULL(arrayRef, CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: kAudioDeviceCustomPropertyAppVolumes cannot be set to NULL");
      ThrowIf(CFGetTypeID(arrayRef) != CFArrayGetTypeID(), CAException(kAudioHardwareIllegalOperationError), "EQM_Device::Device_SetPropertyData: CFType given for kAudioDeviceCustomPropertyAppVolumes was not a CFArray");
      
      CACFArray array(arrayRef, false);
      
      bool propertyWasChanged = false;
      
      CAMutex::Locker theStateLocker(mStateMutex);
      
      try
      {
        propertyWasChanged = mClients.SetClientsRelativeVolumes(array);
      }
      catch(EQM_InvalidClientRelativeVolumeException)
      {
        Throw(CAException(kAudioHardwareIllegalOperationError));
      }
      
      if(propertyWasChanged)
      {
        // Send notification
        CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
          AudioObjectPropertyAddress theChangedProperties[] = { kEQMAppVolumesAddress };
          EQM_PlugIn::Host_PropertiesChanged(inObjectID, 1, theChangedProperties);
        });
      }
    }
      break;
      
    case kAudioDeviceCustomPropertyEnabledOutputControls:
    {
      ThrowIf(inDataSize < sizeof(CFArrayRef),
              CAException(kAudioHardwareBadPropertySizeError),
              "EQM_Device::Device_SetPropertyData: wrong size for the data for "
              "kAudioDeviceCustomPropertyEnabledOutputControls");
      
      CFArrayRef theEnabledControlsRef = *reinterpret_cast<const CFArrayRef*>(inData);
      
      ThrowIfNULL(theEnabledControlsRef,
                  CAException(kAudioHardwareIllegalOperationError),
                  "EQM_Device::Device_SetPropertyData: null reference given for "
                  "kAudioDeviceCustomPropertyEnabledOutputControls");
      ThrowIf(CFGetTypeID(theEnabledControlsRef) != CFArrayGetTypeID(),
              CAException(kAudioHardwareIllegalOperationError),
              "EQM_Device::Device_SetPropertyData: CFType given for "
              "kAudioDeviceCustomPropertyEnabledOutputControls was not a CFArray");
      
      CACFArray theEnabledControls(theEnabledControlsRef, false);
      
      ThrowIf(theEnabledControls.GetNumberItems() != 2,
              CAException(kAudioHardwareIllegalOperationError),
              "EQM_Device::Device_SetPropertyData: Expected the CFArray given for "
              "kAudioDeviceCustomPropertyEnabledOutputControls to have exactly 2 elements");
      
      bool theVolumeControlEnabled;
      bool didGetBool = theEnabledControls.GetBool(kEQMEnabledOutputControlsIndex_Volume,
                                                   theVolumeControlEnabled);
      ThrowIf(!didGetBool,
              CAException(kAudioHardwareIllegalOperationError),
              "EQM_Device::Device_SetPropertyData: Expected CFBoolean for volume elem of "
              "kAudioDeviceCustomPropertyEnabledOutputControls");
      
      bool theMuteControlEnabled;
      didGetBool = theEnabledControls.GetBool(kEQMEnabledOutputControlsIndex_Mute,
                                              theMuteControlEnabled);
      ThrowIf(!didGetBool,
              CAException(kAudioHardwareIllegalOperationError),
              "EQM_Device::Device_SetPropertyData: Expected CFBoolean for mute elem of "
              "kAudioDeviceCustomPropertyEnabledOutputControls");
      
      RequestEnabledControls(theVolumeControlEnabled, theMuteControlEnabled);
    }
      break;
      
    default:
      EQM_AbstractDevice::SetPropertyData(inObjectID, inClientPID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData);
      break;
  };
}

#pragma mark IO Operations

void	EQM_Device::StartIO(UInt32 inClientID)
{
  bool clientIsEQMApp, EQMAppHasClientRegistered;
  
  {
    CAMutex::Locker theStateLocker(mStateMutex);
    
    // An overview of the process this function is part of:
    //   - A client starts IO.
    //   - The plugin host (the HAL) calls the StartIO function in EQM_PlugInInterface, which calls this function.
    //   - EQMDriver sends a message to EQMApp telling it to start the (real) audio hardware.
    //   - EQMApp starts the hardware and, after the hardware is ready, replies to EQMDriver's message.
    //   - EQMDriver lets the host know that it's ready to do IO by returning from StartIO.
    
    // Update our client data.
    //
    // We add the work to the task queue, rather than doing it here, because BeginIOOperation and EndIOOperation
    // also add this task to the queue and the updates should be done in order.
    bool didStartIO = mTaskQueue.QueueSync_StartClientIO(&mClients, inClientID);
    
    // We only tell the hardware to start if this is the first time IO has been started.
    if(didStartIO)
    {
      kern_return_t theError = _HW_StartIO();
      ThrowIfKernelError(theError,
                         CAException(theError),
                         "EQM_Device::StartIO: Failed to start because of an error calling down to the driver.");
    }
    
    clientIsEQMApp = mClients.IsEQMApp(inClientID);
    EQMAppHasClientRegistered = mClients.EQMAppHasClientRegistered();
  }
  
  // We only return from StartIO after EQMApp is ready to pass the audio through to the output device. That way
  // the HAL doesn't start sending us data before EQMApp can play it, which would mean we'd have to either drop
  // frames or increase latency.
  if(!clientIsEQMApp && EQMAppHasClientRegistered)
  {
    UInt64 theXPCError = StartEQMAppPlayThroughSync(GetObjectID() == kObjectID_Device_UI_Sounds);
    
    switch(theXPCError)
    {
      case kEQMXPC_Success:
        DebugMsg("EQM_Device::StartIO: Ready for IO.");
        break;
        
      case kEQMXPC_MessageFailure:
        // This most likely means EQMXPCHelper isn't installed or has crashed. IO will probably still work,
        // but we may drop frames while the audio hardware starts up.
        LogError("EQM_Device::StartIO: Couldn't reach EQMApp via XPC. Attempting to start IO anyway.");
        break;
        
      case kEQMXPC_ReturningEarlyError:
        // This can (and might always) happen when the user changes output device in EQMApp while IO is running.
        // See EQMAudioDeviceManager::startPlayThroughSync and EQMPlayThrough::WaitForOutputDeviceToStart.
        LogWarning("EQM_Device::StartIO: EQMApp was busy, so EQMDriver has to return from StartIO early.");
        break;
        
      default:
        LogError("EQM_Device::StartIO: EQMApp failed to start the output device. theXPCError=%llu", theXPCError);
        Throw(CAException(kAudioHardwareNotRunningError));
    }
  }
}

void	EQM_Device::StopIO(UInt32 inClientID)
{
  CAMutex::Locker theStateLocker(mStateMutex);
  
  // Update our client data.
  //
  // We add the work to the task queue, rather than doing it here, because BeginIOOperation and EndIOOperation also
  // add this task to the queue and the updates should be done in order.
  bool didStopIO = mTaskQueue.QueueSync_StopClientIO(&mClients, inClientID);
  
  //	we tell the hardware to stop if this is the last stop call
  if(didStopIO)
  {
    _HW_StopIO();
  }
}

void	EQM_Device::GetZeroTimeStamp(Float64& outSampleTime, UInt64& outHostTime, UInt64& outSeed)
{
  // accessing the buffers requires holding the IO mutex
  CAMutex::Locker theIOLocker(mIOMutex);
  
  if(mWrappedAudioEngine != NULL)
  {
  }
  else
  {
    // Without a wrapped device, we base our timing on the host. This is mostly from Apple's NullAudio.c sample code
    UInt64 theCurrentHostTime;
    Float64 theHostTicksPerRingBuffer;
    Float64 theHostTickOffset;
    UInt64 theNextHostTime;
    
    //	get the current host time
    theCurrentHostTime = mach_absolute_time();
    
    //	calculate the next host time
    theHostTicksPerRingBuffer = mLoopbackTime.hostTicksPerFrame * kLoopbackRingBufferFrameSize;
    theHostTickOffset = static_cast<Float64>(mLoopbackTime.numberTimeStamps + 1) * theHostTicksPerRingBuffer;
    theNextHostTime = mLoopbackTime.anchorHostTime + static_cast<UInt64>(theHostTickOffset);
    
    //	go to the next time if the next host time is less than the current time
    if(theNextHostTime <= theCurrentHostTime)
    {
      mLoopbackTime.numberTimeStamps++;
    }
    
    //	set the return values
    outSampleTime = mLoopbackTime.numberTimeStamps * kLoopbackRingBufferFrameSize;
    outHostTime = static_cast<UInt64>(mLoopbackTime.anchorHostTime + (static_cast<Float64>(mLoopbackTime.numberTimeStamps) * theHostTicksPerRingBuffer));
    // TODO: I think we should increment outSeed whenever this device switches to/from having a wrapped engine
    outSeed = 1;
  }
}

void	EQM_Device::WillDoIOOperation(UInt32 inOperationID, bool& outWillDo, bool& outWillDoInPlace) const
{
  switch(inOperationID)
  {
    case kAudioServerPlugInIOOperationThread:
    case kAudioServerPlugInIOOperationReadInput:
    case kAudioServerPlugInIOOperationProcessOutput:
    case kAudioServerPlugInIOOperationWriteMix:
      outWillDo = true;
      outWillDoInPlace = true;
      break;
      
    case kAudioServerPlugInIOOperationProcessMix:
      outWillDo = mVolumeControl.WillApplyVolumeToAudioRT();
      outWillDoInPlace = true;
      break;
      
    case kAudioServerPlugInIOOperationCycle:
    case kAudioServerPlugInIOOperationConvertInput:
    case kAudioServerPlugInIOOperationProcessInput:
    case kAudioServerPlugInIOOperationMixOutput:
    case kAudioServerPlugInIOOperationConvertMix:
    default:
      outWillDo = false;
      outWillDoInPlace = true;
      break;
      
  };
}

void	EQM_Device::BeginIOOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, UInt32 inClientID)
{
#pragma unused(inIOBufferFrameSize, inIOCycleInfo)
  
  if(inOperationID == kAudioServerPlugInIOOperationThread)
  {
    // Update this client's IO state and send notifications if that changes the value of
    // kAudioDeviceCustomPropertyDeviceIsRunning or
    // kAudioDeviceCustomPropertyDeviceIsRunningSomewhereOtherThanEQMApp. We have to do this here
    // as well as in StartIO because the HAL only calls StartIO/StopIO with the first/last clients.
    //
    // We perform the update async because it isn't real-time safe, but we can't just dispatch it with
    // dispatch_async because that isn't real-time safe either. (Apparently even constructing a block
    // isn't.)
    //
    // We don't have to hold the IO mutex here because mTaskQueue and mClients don't change and
    // adding a task to mTaskQueue is thread safe.
    mTaskQueue.QueueAsync_StartClientIO(&mClients, inClientID);
  }
}

void	EQM_Device::DoIOOperation(AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
#pragma unused(inStreamObjectID, ioSecondaryBuffer)
  
  switch(inOperationID)
  {
    case kAudioServerPlugInIOOperationReadInput:
    {
      CAMutex::Locker theIOLocker(mIOMutex);
      ReadInputData(inIOBufferFrameSize, inIOCycleInfo.mInputTime.mSampleTime, ioMainBuffer);
    }
      break;
      
    case kAudioServerPlugInIOOperationProcessOutput:
    {
      bool theClientIsMusicPlayer = mClients.IsMusicPlayerRT(inClientID);
      
      CAMutex::Locker theIOLocker(mIOMutex);
      // Called in this IO operation so we can get the music player client's data separately
      mAudibleState.UpdateWithClientIO(theClientIsMusicPlayer,
                                       inIOBufferFrameSize,
                                       inIOCycleInfo.mOutputTime.mSampleTime,
                                       reinterpret_cast<const Float32*>(ioMainBuffer));
    }
      ApplyClientRelativeVolume(inClientID, inIOBufferFrameSize, ioMainBuffer);
      break;
      
    case kAudioServerPlugInIOOperationProcessMix:
    {
      // Check the arguments.
      ThrowIfNULL(ioMainBuffer,
                  CAException(kAudioHardwareIllegalOperationError),
                  "EQM_Device::DoIOOperation: Buffer for "
                  "kAudioServerPlugInIOOperationProcessMix must not be null");
      
      CAMutex::Locker theIOLocker(mIOMutex);
      
      // We ask to do this IO operation so this device can apply its own volume to the
      // stream. Currently, only the UI sounds device does.
      mVolumeControl.ApplyVolumeToAudioRT(reinterpret_cast<Float32*>(ioMainBuffer),
                                          inIOBufferFrameSize);
    }
      break;
      
    case kAudioServerPlugInIOOperationWriteMix:
    {
      CAMutex::Locker theIOLocker(mIOMutex);
      
      bool didChangeState =
      mAudibleState.UpdateWithMixedIO(inIOBufferFrameSize,
                                      inIOCycleInfo.mOutputTime.mSampleTime,
                                      reinterpret_cast<const Float32*>(ioMainBuffer));
      
      if(didChangeState)
      {
        // Send notifications. I'm pretty sure we don't have to use
        // RequestDeviceConfigurationChange for this property, but the docs seemed a bit
        // unclear to me.
        mTaskQueue.QueueAsync_SendPropertyNotification(
                                                       kAudioDeviceCustomPropertyDeviceAudibleState, GetObjectID());
      }
      
      WriteOutputData(inIOBufferFrameSize, inIOCycleInfo.mOutputTime.mSampleTime, ioMainBuffer);
    }
      break;
      
    default:
      // Note that this will only log the error in debug builds.
      DebugMsg("EQM_Device::DoIOOperation: Unexpected IO operation: %u", inOperationID);
      break;
  };
}

void	EQM_Device::EndIOOperation(UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo& inIOCycleInfo, UInt32 inClientID)
{
#pragma unused(inIOBufferFrameSize, inIOCycleInfo)
  
  if(inOperationID == kAudioServerPlugInIOOperationThread)
  {
    // Tell EQM_Clients that this client has stopped IO. Queued async because we have to be real-time safe here.
    //
    // We don't have to hold the IO mutex here because mTaskQueue and mClients don't change and adding a task to
    // mTaskQueue is thread safe.
    mTaskQueue.QueueAsync_StopClientIO(&mClients, inClientID);
  }
}

void	EQM_Device::ReadInputData(UInt32 inIOBufferFrameSize, Float64 inSampleTime, void* outBuffer)
{
  //	figure out where we are starting
  UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
  UInt32 theStartFrameOffset = theSampleTime % kLoopbackRingBufferFrameSize;
  
  //	figure out how many frames we need to copy
  UInt32 theNumberFramesToCopy1 = inIOBufferFrameSize;
  UInt32 theNumberFramesToCopy2 = 0;
  if((theStartFrameOffset + theNumberFramesToCopy1) > kLoopbackRingBufferFrameSize)
  {
    theNumberFramesToCopy1 = kLoopbackRingBufferFrameSize - theStartFrameOffset;
    theNumberFramesToCopy2 = inIOBufferFrameSize - theNumberFramesToCopy1;
  }
  
  //	do the copying (the byte sizes here assume a 32 bit stereo sample format)
  Float32* theDestination = reinterpret_cast<Float32*>(outBuffer);
  memcpy(theDestination, mLoopbackRingBuffer + (theStartFrameOffset * 2), theNumberFramesToCopy1 * 8);
  if(theNumberFramesToCopy2 > 0)
  {
    memcpy(theDestination + (theNumberFramesToCopy1 * 2), mLoopbackRingBuffer, theNumberFramesToCopy2 * 8);
  }
  
  //DebugMsg("EQM_Device::ReadInputData: Reading. theSampleTime=%llu theStartFrameOffset=%u theNumberFramesToCopy1=%u theNumberFramesToCopy2=%u", theSampleTime, theStartFrameOffset, theNumberFramesToCopy1, theNumberFramesToCopy2);
}

void	EQM_Device::WriteOutputData(UInt32 inIOBufferFrameSize, Float64 inSampleTime, const void* inBuffer)
{
  //	figure out where we are starting
  UInt64 theSampleTime = static_cast<UInt64>(inSampleTime);
  UInt32 theStartFrameOffset = theSampleTime % kLoopbackRingBufferFrameSize;
  
  //	figure out how many frames we need to copy
  UInt32 theNumberFramesToCopy1 = inIOBufferFrameSize;
  UInt32 theNumberFramesToCopy2 = 0;
  if((theStartFrameOffset + theNumberFramesToCopy1) > kLoopbackRingBufferFrameSize)
  {
    theNumberFramesToCopy1 = kLoopbackRingBufferFrameSize - theStartFrameOffset;
    theNumberFramesToCopy2 = inIOBufferFrameSize - theNumberFramesToCopy1;
  }
  
  //	do the copying (the byte sizes here assume a 32 bit stereo sample format)
  const Float32* theSource = reinterpret_cast<const Float32*>(inBuffer);
  memcpy(mLoopbackRingBuffer + (theStartFrameOffset * 2), theSource, theNumberFramesToCopy1 * 8);
  if(theNumberFramesToCopy2 > 0)
  {
    memcpy(mLoopbackRingBuffer, theSource + (theNumberFramesToCopy1 * 2), theNumberFramesToCopy2 * 8);
  }
  
  //DebugMsg("EQM_Device::WriteOutputData: Writing. theSampleTime=%llu theStartFrameOffset=%u theNumberFramesToCopy1=%u theNumberFramesToCopy2=%u", theSampleTime, theStartFrameOffset, theNumberFramesToCopy1, theNumberFramesToCopy2);
}

void	EQM_Device::ApplyClientRelativeVolume(UInt32 inClientID, UInt32 inIOBufferFrameSize, void* ioBuffer) const
{
  Float32* theBuffer = reinterpret_cast<Float32*>(ioBuffer);
  Float32 theRelativeVolume = mClients.GetClientRelativeVolumeRT(inClientID);
  
  auto thePanPositionInt = mClients.GetClientPanPositionRT(inClientID);
  Float32 thePanPosition = static_cast<Float32>(thePanPositionInt) / 100.0f;
  
  // TODO When we get around to supporting devices with more than two channels it would be worth looking into
  //      kAudioFormatProperty_PanningMatrix and kAudioFormatProperty_BalanceFade in AudioFormat.h.
  
  // TODO precompute matrix coefficients w/ volume and do everything in one pass
  
  // Apply balance w/ crossfeed to the frames in the buffer.
  // Expect samples interleaved, starting with left
  if (thePanPosition > 0.0f) {
    for (UInt32 i = 0; i < inIOBufferFrameSize * 2; i += 2) {
      auto L = i;
      auto R = i + 1;
      
      theBuffer[R] = theBuffer[R] + theBuffer[L] * thePanPosition;
      theBuffer[L] = theBuffer[L] * (1 - thePanPosition);
    }
  } else if (thePanPosition < 0.0f) {
    for (UInt32 i = 0; i < inIOBufferFrameSize * 2; i += 2) {
      auto L = i;
      auto R = i + 1;
      
      theBuffer[L] = theBuffer[L] + theBuffer[R] * (-thePanPosition);
      theBuffer[R] = theBuffer[R] * (1 + thePanPosition);
    }
  }
  
  if(theRelativeVolume != 1.0f)
  {
    for(UInt32 i = 0; i < inIOBufferFrameSize * 2; i++)
    {
      Float32 theAdjustedSample = theBuffer[i] * theRelativeVolume;
      
      // Clamp to [-1, 1].
      // (This way is roughly 6 times faster than using std::min and std::max because the compiler can vectorize the loop.)
      const Float32 theAdjustedSampleClippedBelow = theAdjustedSample < -1.0f ? -1.0f : theAdjustedSample;
      theBuffer[i] = theAdjustedSampleClippedBelow > 1.0f ? 1.0f : theAdjustedSampleClippedBelow;
    }
  }
}

#pragma mark Accessors

void    EQM_Device::RequestEnabledControls(bool inVolumeEnabled, bool inMuteEnabled)
{
  CAMutex::Locker theStateLocker(mStateMutex);
  
  bool changeVolume = (mVolumeControl.IsActive() != inVolumeEnabled);
  bool changeMute = (mMuteControl.IsActive() != inMuteEnabled);
  
  if(changeVolume)
  {
    DebugMsg("EQM_Device::RequestEnabledControls: %s volume control",
             (inVolumeEnabled ? "Enabling" : "Disabling"));
    mPendingOutputVolumeControlEnabled = inVolumeEnabled;
  }
  
  if(changeMute)
  {
    DebugMsg("EQM_Device::RequestEnabledControls: %s mute control",
             (inMuteEnabled ? "Enabling" : "Disabling"));
    mPendingOutputMuteControlEnabled = inMuteEnabled;
  }
  
  if(changeVolume || changeMute)
  {
    // Ask the host to stop IO (and whatever else) so we can safely update the device's list of
    // controls. See RequestDeviceConfigurationChange in AudioServerPlugIn.h.
    AudioObjectID theDeviceObjectID = GetObjectID();
    UInt64 action = static_cast<UInt64>(ChangeAction::SetEnabledControls);
    
    CADispatchQueue::GetGlobalSerialQueue().Dispatch(false,	^{
      EQM_PlugIn::Host_RequestDeviceConfigurationChange(theDeviceObjectID, action, nullptr);
    });
  }
}

Float64  EQM_Device::GetSampleRate() const
{
  // The sample rate is guarded by the state lock. Note that we don't need to take the IO lock.
  CAMutex::Locker theStateLocker(mStateMutex);
  
  Float64 theSampleRate;
  
  // Report the sample rate from the wrapped device if we have one. Note that _HW_GetSampleRate
  // the device's nominal sample rate, not one calculated from its timestamps.
  if(mWrappedAudioEngine == nullptr)
  {
    theSampleRate = mLoopbackSampleRate;
  }
  else
  {
    theSampleRate = _HW_GetSampleRate();
  }
  
  return theSampleRate;
}

UInt32  EQM_Device::GetSafetyOffset() const
{
  // The safety offset is guarded by the state lock. Note that we don't need to take the IO lock.
  CAMutex::Locker theStateLocker(mStateMutex);
  return mSafetyOffset;
}

UInt32  EQM_Device::GetLatency() const
{
  // The safety offset is guarded by the state lock. Note that we don't need to take the IO lock.
  CAMutex::Locker theStateLocker(mStateMutex);
  
  return mLatency;
}

void	EQM_Device::RequestSampleRate(Float64 inRequestedSampleRate)
{
  // Changing the sample rate needs to be handled via the RequestConfigChange/PerformConfigChange
  // machinery. See RequestDeviceConfigurationChange in AudioServerPlugIn.h.
  
  // We try to support any sample rate a real output device might.
  ThrowIf(inRequestedSampleRate < 1.0,
          CAException(kAudioDeviceUnsupportedFormatError),
          "EQM_Device::RequestSampleRate: unsupported sample rate");
  
  DebugMsg("EQM_Device::RequestSampleRate: Sample rate change requested: %f",
           inRequestedSampleRate);
  
  CAMutex::Locker theStateLocker(mStateMutex);
  
  if(inRequestedSampleRate != GetSampleRate())  // Check the sample rate will actually be changed.
  {
    mPendingSampleRate = inRequestedSampleRate;
    
    // Dispatch this so the change can happen asynchronously.
    auto requestSampleRate = ^{
      UInt64 action = static_cast<UInt64>(ChangeAction::SetSampleRate);
      EQM_PlugIn::Host_RequestDeviceConfigurationChange(GetObjectID(), action, nullptr);
    };
    
    CADispatchQueue::GetGlobalSerialQueue().Dispatch(false, requestSampleRate);
  }
}

EQM_Object&  EQM_Device::GetOwnedObjectByID(AudioObjectID inObjectID)
{
  // C++ is weird. See "Avoid Duplication in const and Non-const Member Functions" in Item 3 of Effective C++.
  return const_cast<EQM_Object&>(static_cast<const EQM_Device&>(*this).GetOwnedObjectByID(inObjectID));
}

const EQM_Object&  EQM_Device::GetOwnedObjectByID(AudioObjectID inObjectID) const
{
  if(inObjectID == mInputStream.GetObjectID())
  {
    return mInputStream;
  }
  else if(inObjectID == mOutputStream.GetObjectID())
  {
    return mOutputStream;
  }
  else if(inObjectID == mVolumeControl.GetObjectID())
  {
    return mVolumeControl;
  }
  else if(inObjectID == mMuteControl.GetObjectID())
  {
    return mMuteControl;
  }
  else
  {
    LogError("EQM_Device::GetOwnedObjectByID: Unknown object ID. inObjectID = %u", inObjectID);
    Throw(CAException(kAudioHardwareBadObjectError));
  }
}

UInt32	EQM_Device::GetNumberOfSubObjects() const
{
  return kNumberOfInputSubObjects + GetNumberOfOutputSubObjects();
}

UInt32	EQM_Device::GetNumberOfOutputSubObjects() const
{
  return kNumberOfOutputStreams + GetNumberOfOutputControls();
}

UInt32	EQM_Device::GetNumberOfOutputControls() const
{
  CAMutex::Locker theStateLocker(mStateMutex);
  
  UInt32 theAnswer = 0;
  
  if(mVolumeControl.IsActive())
  {
    theAnswer++;
  }
  
  if(mMuteControl.IsActive())
  {
    theAnswer++;
  }
  
  return theAnswer;
}

void    EQM_Device::SetEnabledControls(bool inVolumeEnabled, bool inMuteEnabled)
{
  CAMutex::Locker theStateLocker(mStateMutex);
  
  if(mVolumeControl.IsActive() != inVolumeEnabled)
  {
    DebugMsg("EQM_Device::SetEnabledControls: %s the volume control",
             inVolumeEnabled ? "Enabling" : "Disabling");
    
    if(inVolumeEnabled)
    {
      mVolumeControl.Activate();
    }
    else
    {
      mVolumeControl.Deactivate();
    }
  }
  
  if(mMuteControl.IsActive() != inMuteEnabled)
  {
    DebugMsg("EQM_Device::SetEnabledControls: %s the mute control",
             inMuteEnabled ? "Enabling" : "Disabling");
    
    if(inMuteEnabled)
    {
      mMuteControl.Activate();
    }
    else
    {
      mMuteControl.Deactivate();
    }
  }
}

void  EQM_Device::SetSampleRate(Float64 inSampleRate, bool force)
{
  // We try to support any sample rate a real output device might.
  ThrowIf(inSampleRate < 1.0,
          CAException(kAudioDeviceUnsupportedFormatError),
          "EQM_Device::SetSampleRate: unsupported sample rate");
  
  CAMutex::Locker theStateLocker(mStateMutex);
  
  Float64 theCurrentSampleRate = GetSampleRate();
  
  if((inSampleRate != theCurrentSampleRate) || force)  // Check whether we need to change it.
  {
    DebugMsg("EQM_Device::SetSampleRate: Changing the sample rate from %f to %f",
             theCurrentSampleRate,
             inSampleRate);
    
    // Update the sample rate on the wrapped device if we have one.
    if(mWrappedAudioEngine != nullptr)
    {
      kern_return_t theError = _HW_SetSampleRate(inSampleRate);
      ThrowIfKernelError(theError,
                         CAException(kAudioHardwareUnspecifiedError),
                         "EQM_Device::SetSampleRate: Error setting the sample rate on the "
                         "wrapped audio device.");
    }
    
    // Update the sample rate for loopback.
    mLoopbackSampleRate = inSampleRate;
    InitLoopback();
    
    // Update the streams.
    mInputStream.SetSampleRate(inSampleRate);
    mOutputStream.SetSampleRate(inSampleRate);
  }
  else
  {
    DebugMsg("EQM_Device::SetSampleRate: The sample rate is already set to %f", inSampleRate);
  }
}

void  EQM_Device::SetSafetyOffset(UInt32 inSafetyOffset)
{
  ThrowIf(inSafetyOffset < 0,
          CAException(kAudioDeviceUnsupportedFormatError),
          "EQM_Device::SetSafetyOffset: unsupported safety offset");
  
  CAMutex::Locker theStateLocker(mStateMutex);

  mSafetyOffset = inSafetyOffset;
}

void  EQM_Device::SetLatency(UInt32 inLatency)
{
  ThrowIf(inLatency < 0,
          CAException(kAudioDeviceUnsupportedFormatError),
          "EQM_Device::SetLatency: unsupported latency");
  
  CAMutex::Locker theStateLocker(mStateMutex);
  
  mLatency = inLatency;
}

bool    EQM_Device::IsStreamID(AudioObjectID inObjectID) const noexcept
{
  return (inObjectID == mInputStream.GetObjectID()) || (inObjectID == mOutputStream.GetObjectID());
}

#pragma mark Hardware Accessors

// TODO: Out of laziness, some of these hardware functions do more than their names suggest

void	EQM_Device::_HW_Open()
{
}

void	EQM_Device::_HW_Close()
{
}

kern_return_t	EQM_Device::_HW_StartIO()
{
  EQMAssert(mStateMutex.IsOwnedByCurrentThread(),
            "EQM_Device::_HW_StartIO: Called without taking the state mutex");
  
  if(mWrappedAudioEngine != nullptr)
  {
  }
  
  // Reset the loopback timing values
  mLoopbackTime.numberTimeStamps = 0;
  mLoopbackTime.anchorHostTime = mach_absolute_time();
  // ...and the most-recent audible/silent sample times. mAudibleState is usually guarded by the
  // IO mutex, but we haven't started IO yet (and this function can only be called by one thread
  // at a time).
  EQMAssert(mIOMutex.IsFree(), "EQM_Device::_HW_StartIO: IO mutex taken before starting IO");
  mAudibleState.Reset();
  
  return KERN_SUCCESS;
}

void	EQM_Device::_HW_StopIO()
{
  if(mWrappedAudioEngine != NULL)
  {
  }
}

Float64	EQM_Device::_HW_GetSampleRate() const
{
  // This function should only be called when wrapping a device.
  ThrowIf(mWrappedAudioEngine == nullptr,
          CAException(kAudioHardwareUnspecifiedError),
          "EQM_Device::_HW_GetSampleRate: No wrapped audio device");
  
  return mWrappedAudioEngine->GetSampleRate();
}

kern_return_t	EQM_Device::_HW_SetSampleRate(Float64 inNewSampleRate)
{
  // This function should only be called when wrapping a device.
  ThrowIf(mWrappedAudioEngine == nullptr,
          CAException(kAudioHardwareUnspecifiedError),
          "EQM_Device::_HW_SetSampleRate: No wrapped audio device");
  
  return mWrappedAudioEngine->SetSampleRate(inNewSampleRate);
}

UInt32	EQM_Device::_HW_GetRingBufferFrameSize() const
{
  return (mWrappedAudioEngine != NULL) ? mWrappedAudioEngine->GetSampleBufferFrameSize() : 0;
}

#pragma mark Implementation

void	EQM_Device::AddClient(const AudioServerPlugInClientInfo* inClientInfo)
{
  DebugMsg("EQM_Device::AddClient: Adding client %u (%s)",
           inClientInfo->mClientID,
           (inClientInfo->mBundleID == NULL ?
            "no bundle ID" :
            CFStringGetCStringPtr(inClientInfo->mBundleID, kCFStringEncodingUTF8)));
  
  CAMutex::Locker theStateLocker(mStateMutex);
  
  mClients.AddClient(inClientInfo);
}

void	EQM_Device::RemoveClient(const AudioServerPlugInClientInfo* inClientInfo)
{
  DebugMsg("EQM_Device::RemoveClient: Removing client %u (%s)",
           inClientInfo->mClientID,
           CFStringGetCStringPtr(inClientInfo->mBundleID, kCFStringEncodingUTF8));
  
  CAMutex::Locker theStateLocker(mStateMutex);
  
  // If we're removing EQMApp, reenable all of EQMDevice's controls.
  if(mClients.IsEQMApp(inClientInfo->mClientID))
  {
    RequestEnabledControls(true, true);
  }
  
  mClients.RemoveClient(inClientInfo->mClientID);
}

void	EQM_Device::PerformConfigChange(UInt64 inChangeAction, void* inChangeInfo)
{
#pragma unused(inChangeInfo)
  DebugMsg("EQM_Device::PerformConfigChange: inChangeAction = %llu", inChangeAction);
  
  // Apply a change requested with EQM_PlugIn::Host_RequestDeviceConfigurationChange. See
  // PerformDeviceConfigurationChange in AudioServerPlugIn.h.
  
  switch(static_cast<ChangeAction>(inChangeAction))
  {
    case ChangeAction::SetSampleRate:
      SetSampleRate(mPendingSampleRate);
      break;
      
    case ChangeAction::SetEnabledControls:
      SetEnabledControls(mPendingOutputVolumeControlEnabled,
                         mPendingOutputMuteControlEnabled);
      break;
  }
}

void	EQM_Device::AbortConfigChange(UInt64 inChangeAction, void* inChangeInfo)
{
#pragma unused(inChangeAction, inChangeInfo)
  
  //	this device doesn't need to do anything special if a change request gets aborted
}

