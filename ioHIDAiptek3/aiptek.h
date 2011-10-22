/*
 *  aiptek.h
 *  hyperpenDaemon
 *
 *  Created by Paar on 21.10.11.
 *  Copyright 2011 Udo Killermann. All rights reserved.
 *
 */

/*
 * Aiptek status packet:
 *
 * (returned as Report 1 - relative coordinates from mouse and stylus)
 *
 *        bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 * byte0   0     0     0     0     0     0     0     1
 * byte1   0     0     0     0     0    BS2   BS    Tip
 * byte2  X7    X6    X5    X4    X3    X2    X1    X0
 * byte3  Y7    Y6    Y5    Y4    Y3    Y2    Y1    Y0
 *
 * (returned as Report 2 - absolute coordinates from the stylus)
 *
 *        bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 * byte0   0     0     0     0     0     0     1     0
 * byte1  X7    X6    X5    X4    X3    X2    X1    X0
 * byte2  X15   X14   X13   X12   X11   X10   X9    X8
 * byte3  Y7    Y6    Y5    Y4    Y3    Y2    Y1    Y0
 * byte4  Y15   Y14   Y13   Y12   Y11   Y10   Y9    Y8
 * byte5   *     *     *    BS2   BS1   Tip   IR    DV
 * byte6  P7    P6    P5    P4    P3    P2    P1    P0
 * byte7  P15   P14   P13   P12   P11   P10   P9    P8
 *
 * (returned as Report 3 - absolute coordinates from the mouse)
 *
 *        bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 * byte0   0     0     0     0     0     0     1     0
 * byte1  X7    X6    X5    X4    X3    X2    X1    X0
 * byte2  X15   X14   X13   X12   X11   X10   X9    X8
 * byte3  Y7    Y6    Y5    Y4    Y3    Y2    Y1    Y0
 * byte4  Y15   Y14   Y13   Y12   Y11   Y10   Y9    Y8
 * byte5   *     *     *    BS2   BS1   Tip   IR    DV
 * byte6  P7    P6    P5    P4    P3    P2    P1    P0
 * byte7  P15   P14   P13   P12   P11   P10   P9    P8
 *
 * (returned as Report 4 - macrokeys from the stylus)
 *
 *        bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 * byte0   0     0     0     0     0     1     0     0
 * byte1   0     0     0    BS2   BS    Tip   IR    DV
 * byte2   0     0     0     0     0     0     1     0
 * byte3   0     0     0    K4    K3    K2    K1    K0
 * byte4  P7    P6    P5    P4    P3    P2    P1    P0
 * byte5  P15   P14   P13   P12   P11   P10   P9    P8
 *
 * (returned as Report 5 - macrokeys from the mouse)
 *
 *        bit7  bit6  bit5  bit4  bit3  bit2  bit1  bit0
 * byte0   0     0     0     0     0     1     0     0
 * byte1   0     0     0    BS2   BS    Tip   IR    DV
 * byte2   0     0     0     0     0     0     1     0
 * byte3   0     0     0    K4    K3    K2    K1    K0
 * byte4  P7    P6    P5    P4    P3    P2    P1    P0
 * byte5  P15   P14   P13   P12   P11   P10   P9    P8
 *
 * IR: In Range = Proximity on
 * DV = Data Valid
 * BS = Barrel Switch (as in, macro keys)
 * BS2 also referred to as Tablet Pick
 *
 * Command Summary:
 *
 * Use report_type CONTROL (3)
 * Use report_id   2
 *
 * Command/Data    Description     Return Bytes    Return Value
 * 0x10/0x00       SwitchToMouse       0
 * 0x10/0x01       SwitchToTablet      0
 * 0x18/0x04       SetResolution       0 
 * 0x12/0xFF       AutoGainOn          0
 * 0x17/0x00       FilterOn            0
 * 0x01/0x00       GetXExtension       2           MaxX
 * 0x01/0x01       GetYExtension       2           MaxY
 * 0x02/0x00       GetModelCode        2           ModelCode = LOBYTE
 * 0x03/0x00       GetODMCode          2           ODMCode
 * 0x08/0x00       GetPressureLevels   2           =512
 * 0x04/0x00       GetFirmwareVersion  2           Firmware Version
 * 0x11/0x02       EnableMacroKeys     0
 *
 * To initialize the tablet:
 *
 * (1) Send Resolution500LPI (Command)
 * (2) Query for Model code (Option Report)
 * (3) Query for ODM code (Option Report)
 * (4) Query for firmware (Option Report)
 * (5) Query for GetXExtension (Option Report)
 * (6) Query for GetYExtension (Option Report)
 * (7) Query for GetPressureLevels (Option Report)
 * (8) SwitchToTablet for Absolute coordinates, or
 *     SwitchToMouse for Relative coordinates (Command)
 * (9) EnableMacroKeys (Command)
 * (10) FilterOn (Command)
 * (11) AutoGainOn (Command)
 *
 * (Step 9 can be omitted, but you'll then have no function keys.)
 */

