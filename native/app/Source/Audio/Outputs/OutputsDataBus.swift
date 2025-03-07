//
//  OutputsDataBus.swift
//  eqMac
//
//  Created by Romans Kisils on 25/04/2019.
//  Copyright © 2019 Romans Kisils. All rights reserved.
//

import Foundation
import SwiftyJSON
import AMCoreAudio

class OutputsDataBus: DataBus {
  required init(route: String, bridge: Bridge) {
    super.init(route: route, bridge: bridge)
    
    self.on(.GET, "/devices") { _, _ in
      return JSON(Output.allowedDevices.map({ $0.json }))
    }
    
    self.on(.GET, "/selected") { _, _ in
      return [
        "id": Outputs.current
      ]
    }
    
    self.on(.POST, "/selected") { data, _ in
      if let id = data["id"] as? AudioDeviceID {
        if let device = AudioDevice.lookup(by: id) {
            Application.selectOutput(device: device)
            return "Output Selected"
        }
        throw "Device with such ID doesn't exist"
      }
      throw "Invalid Device ID specified"
    }
    
    AudioDeviceEvents.on(.isJackConnectedChanged, sendOutputDevices)
    AudioDeviceEvents.on(.listChanged, sendOutputDevices)
    AudioDeviceEvents.onDeviceListChanged(sendOutputDevices)
    AudioDeviceEvents.on(.outputChanged) { _ in
      if (Application.output != nil) {
        self.send(to: "/selected", data: [ "id": Application.output.device.id ])
      }
    }
  }
  
  func sendOutputDevices (args: Any) {
    self.send(to: "/devices", data: JSON(Output.allowedDevices.map({ $0.json })))
  }
}
