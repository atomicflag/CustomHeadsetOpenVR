import { Component, input } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { CrystalLightConfig } from '../../../services/JsonFileDefines';
import { DeviceConfigComponentBase } from '../DeviceConfigComponentBase';
import { FieldNoteComponent } from '../../../utilities/field-note/field-note.component';
import { FieldTipComponent } from '../../../utilities/field-tip/field-tip.component';
import { ResetButtonComponent } from '../../../utilities/reset-button/reset-button.component';

@Component({
  selector: 'app-local-dimming-settings',
  imports: [
    FormsModule,
    MatSlideToggleModule,
    FieldNoteComponent,
    FieldTipComponent,
    ResetButtonComponent
  ],
  templateUrl: './local-dimming-settings.component.html',
  styleUrl: './local-dimming-settings.component.scss'
})
export class LocalDimmingSettingsComponent extends DeviceConfigComponentBase<CrystalLightConfig> {
  override settingField = input.required<string>();
  override driverName = input<string | string[]>([]);
}
