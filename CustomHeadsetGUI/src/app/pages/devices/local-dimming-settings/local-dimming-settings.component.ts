import { Component, computed, input } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatInputModule } from '@angular/material/input';
import { MatSelectModule } from '@angular/material/select';
import { MatSlideToggleModule } from '@angular/material/slide-toggle';
import { MatSliderModule } from '@angular/material/slider';
import { MatTooltipModule } from '@angular/material/tooltip';
import { CrystalLightConfig } from '../../../services/JsonFileDefines';
import { DeviceConfigComponentBase } from '../DeviceConfigComponentBase';
import { FieldNoteComponent } from '../../../utilities/field-note/field-note.component';
import { FieldTipComponent } from '../../../utilities/field-tip/field-tip.component';
import { ResetButtonComponent } from '../../../utilities/reset-button/reset-button.component';

@Component({
  selector: 'app-local-dimming-settings',
  imports: [
    FormsModule,
    MatButtonModule,
    MatIconModule,
    MatInputModule,
    MatSelectModule,
    MatSlideToggleModule,
    MatSliderModule,
    MatTooltipModule,
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

  // One byte per zone for each eye. The zone data goes out as blocks, so any grid can be sent as several
  // frames, except on the 65 byte param frame without the data transfer sub-header: that frame carries one
  // block only, and its payload area is 60 bytes. With the sub-header the block length is one byte, so a
  // block carries at most 255 data bytes, and the 1024 byte data report carries at most 1021 without it.
  // Matches LocalDimming::DataBlockLimit.
  static readonly maxParamPayload = 60;

  zonesPerEye = computed(() => {
    const grid = this.settings?.localDimming;
    return grid ? grid.gridWidth * grid.gridHeight : 0;
  });
  payloadFits = computed(() => {
    // Only the 65 byte param frame without the sub-header can be too large: it carries one block, so the
    // zone data has to fit in its 60 byte payload area. The sub-header form and the 1024 byte data report
    // both chunk, so any grid is sent as several frames.
    if (this.settings?.localDimming?.useBigReport || this.settings?.localDimming?.useDataHeader) {
      return true;
    }
    return this.zonesPerEye() * 2 <= LocalDimmingSettingsComponent.maxParamPayload;
  });
}
