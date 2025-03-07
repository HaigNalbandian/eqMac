import { Component, OnInit, Input, ViewChild, ElementRef, EventEmitter, Output, NgZone, HostBinding } from '@angular/core'
import { SelectBoxComponent } from '../select-box/select-box.component'
import { UtilitiesService } from '../../services/utilities.service'
import { InputFieldComponent } from '../input-field/input-field.component'
import { FadeInOutAnimation } from 'src/app/modules/animations'

@Component({
  selector: 'eqm-dropdown',
  templateUrl: './dropdown.component.html',
  styleUrls: ['./dropdown.component.scss'],
  animations: [ FadeInOutAnimation ]
})
export class DropdownComponent implements OnInit {
  constructor (private utils: UtilitiesService, private zone: NgZone) { }
  private _items: any[] = []
  hasSelection = false
  @Input() editable = false
  @Input()
  get items () {
    return this._items
  }
  set items (newItems) {
    if (!newItems || !Array.isArray(newItems)) return
    this._items = newItems
    const hasSelection = newItems.length > 1
    this.zone.run(() => this.hasSelection = hasSelection)
  }
  @HostBinding('class.disabled') @Input() disabled = false
  @Input() label: string = null
  @Input() selectedItem = null
  @Input() labelParam = 'text'
  @Input() numberOfVisibleItems = 6
  @Input() placeholder = 'Select item'
  @Input() noItemsPlaceholder = 'No items'
  @Output() itemSelected = new EventEmitter()

  @ViewChild('container', { read: ElementRef, static: true }) container: ElementRef
  @ViewChild('box', { read: ElementRef, static: true }) box: ElementRef
  @ViewChild('box', { static: true }) boxComponent: SelectBoxComponent
  shown = false
  yCoordinate = 0
  direction: 'down' | 'up' = 'down'
  private padding = 5

  async ngOnInit () {
    if (!this.items) this.items = []
    if (!this.selectedItem && this.items.length > 0) {
      this.selectedItem = this.items[0]
      this.label = null
    }
    this.setDimensions()
    this.calculateYCoordinate()
    for (let _ in [...Array(3)]) {
      await this.utils.delay(100)
      this.calculateYCoordinate()
    }
  }

  setDimensions () {
    const inputEl = this.container.nativeElement
    const boxEl = this.box.nativeElement

    const inputWidth = inputEl.offsetWidth

    boxEl.style.width = `${inputWidth}px`
  }

  calculateYCoordinate () {
    const body = document.body
    const html = document.documentElement
    const viewHeight = Math.max(body.scrollHeight, body.offsetHeight, html.clientHeight, html.scrollHeight, html.offsetHeight)
    const preferredDirection = 'down'
    this.direction = preferredDirection
    const inputEl = this.container.nativeElement

    const inputHeight = inputEl.offsetHeight
    const inputPosition = inputEl.getBoundingClientRect()

    const boxHeight = this.boxComponent.height

    const downY = inputPosition.y + inputHeight + this.padding / 2
    const downSpaceLeft = viewHeight - (downY + boxHeight)

    const upY = inputPosition.top - boxHeight - this.padding
    const upSpaceLeft = upY

    this.direction = downSpaceLeft > upSpaceLeft ? 'down' : 'up'
    let y = this.direction === 'down' ? downY : upY

    this.yCoordinate = y
  }

  async toggle (event) {
    event.stopPropagation()
    if (this.shown) {
      this.close()
    } else {
      this.open()
    }
  }

  async open () {
    if (!this.disabled && !this.shown && this.items.length > 1) {
      this.calculateYCoordinate()
      this.setDimensions()
      this.shown = true
    }
  }

  async close () {
    if (!this.disabled && this.shown) {
      this.shown = false
    }
  }

  selectItem (item) {
    this.selectedItem = item
    this.label = null
    this.itemSelected.emit(item)
    this.close()
  }
}
