import { Component, OnInit, Input, Output, EventEmitter } from '@angular/core'

@Component({
  selector: 'eqm-checkbox',
  templateUrl: './checkbox.component.html',
  styleUrls: ['./checkbox.component.scss']
})
export class CheckboxComponent {
  @Input() interactive: boolean = true
  @Input() checked: boolean = false
  @Output() checkedChanged = new EventEmitter<boolean>()

  toggle () {
    if (this.interactive) {
      this.checked = !this.checked
      this.checkedChanged.emit()
    }
  }
}
