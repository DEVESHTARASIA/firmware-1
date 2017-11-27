#include <math.h>
#include "key_action.h"
#include "led_display.h"
#include "layer.h"
#include "usb_interfaces/usb_interface_mouse.h"
#include "keymap.h"
#include "peripherals/test_led.h"
#include "slave_drivers/is31fl3731_driver.h"
#include "slave_drivers/uhk_module_driver.h"
#include "macros.h"
#include "key_states.h"
#include "right_key_matrix.h"
#include "layer.h"
#include "usb_report_updater.h"
#include "timer.h"
#include "key_debouncer.h"
#include "config_parser/parse_keymap.h"

uint32_t UsbReportUpdateTime = 0;
static uint32_t elapsedTime;

static uint16_t doubleTapSwitchLayerTimeout = 300;

static float mouseScrollSpeed = 0.1;

static bool activeMouseStates[ACTIVE_MOUSE_STATES_COUNT];

static mouse_kinetic_state_t mouseMoveState = {
    .upState = SerializedMouseAction_MoveUp,
    .downState = SerializedMouseAction_MoveDown,
    .leftState = SerializedMouseAction_MoveLeft,
    .rightState = SerializedMouseAction_MoveRight,
    .initialSpeed = 100,
    .acceleration = 300,
    .deceleratedSpeed = 250,
    .baseSpeed = 500,
    .acceleratedSpeed = 1000,
};
//static mouse_kinetic_state_t mouseScrollState;

void processMouseKineticState(mouse_kinetic_state_t *kineticState)
{
    if (!kineticState->wasMoveAction) {
        kineticState->currentSpeed = kineticState->initialSpeed;
    }

    bool isMoveAction = activeMouseStates[kineticState->upState] ||
                        activeMouseStates[kineticState->downState] ||
                        activeMouseStates[kineticState->leftState] ||
                        activeMouseStates[kineticState->rightState];

    mouse_speed_t mouseSpeed = MouseSpeed_Normal;
    if (activeMouseStates[SerializedMouseAction_Accelerate]) {
        kineticState->targetSpeed = kineticState->acceleratedSpeed;
        mouseSpeed = MouseSpeed_Accelerated;
    } else if (activeMouseStates[SerializedMouseAction_Decelerate]) {
        kineticState->targetSpeed = kineticState->deceleratedSpeed;
        mouseSpeed = MouseSpeed_Decelerated;
    } else if (isMoveAction) {
        kineticState->targetSpeed = kineticState->baseSpeed;
    }

    if (mouseSpeed == MouseSpeed_Accelerated || (kineticState->wasMoveAction && isMoveAction && (kineticState->prevMouseSpeed != mouseSpeed))) {
        kineticState->currentSpeed = kineticState->targetSpeed;
    }

    if (isMoveAction) {
        if (kineticState->currentSpeed < kineticState->targetSpeed) {
            kineticState->currentSpeed += kineticState->acceleration * elapsedTime / 1000;
            if (kineticState->currentSpeed > kineticState->targetSpeed) {
                kineticState->currentSpeed = kineticState->targetSpeed;
            }
        } else {
            kineticState->currentSpeed -= kineticState->acceleration * elapsedTime / 1000;
            if (kineticState->currentSpeed < kineticState->targetSpeed) {
                kineticState->currentSpeed = kineticState->targetSpeed;
            }
        }

        uint16_t distance = kineticState->currentSpeed * elapsedTime / 1000;

        if (activeMouseStates[kineticState->leftState]) {
            kineticState->xSum -= distance;
        } else if (activeMouseStates[kineticState->rightState]) {
            kineticState->xSum += distance;
        }

        float xSumInt;
        float xSumFrac = modff(kineticState->xSum, &xSumInt);
        kineticState->xSum = xSumFrac;
        kineticState->xOut = xSumInt;

        if (activeMouseStates[kineticState->upState]) {
            kineticState->ySum -= distance;
        } else if (activeMouseStates[kineticState->downState]) {
            kineticState->ySum += distance;
        }

        float ySumInt;
        float ySumFrac = modff(kineticState->ySum, &ySumInt);
        kineticState->ySum = ySumFrac;
        kineticState->yOut = ySumInt;
    }

    kineticState->prevMouseSpeed = mouseSpeed;
    kineticState->wasMoveAction = isMoveAction;
}

void processMouseActions()
{
    processMouseKineticState(&mouseMoveState);
    ActiveUsbMouseReport->x = mouseMoveState.xOut;
    ActiveUsbMouseReport->y = mouseMoveState.yOut;
    mouseMoveState.xOut = 0;
    mouseMoveState.yOut = 0;

    bool isScrollAction = activeMouseStates[SerializedMouseAction_ScrollUp] ||
                          activeMouseStates[SerializedMouseAction_ScrollDown] ||
                          activeMouseStates[SerializedMouseAction_ScrollLeft] ||
                          activeMouseStates[SerializedMouseAction_ScrollRight];

    static float mouseScrollDistanceSum = 0;
    if (isScrollAction) {
        mouseScrollDistanceSum += mouseScrollSpeed;
        float mouseScrollDistanceIntegerSum;
        float mouseScrollDistanceFractionSum = modff(mouseScrollDistanceSum, &mouseScrollDistanceIntegerSum);

        if (mouseScrollDistanceIntegerSum) {
            if (activeMouseStates[SerializedMouseAction_ScrollUp]) {
                ActiveUsbMouseReport->wheelX = mouseScrollDistanceIntegerSum;
            } else if (activeMouseStates[SerializedMouseAction_ScrollDown]) {
                ActiveUsbMouseReport->wheelX = -mouseScrollDistanceIntegerSum;
            }

            if (activeMouseStates[SerializedMouseAction_ScrollRight]) {
                ActiveUsbMouseReport->wheelY = mouseScrollDistanceIntegerSum;
            } else if (activeMouseStates[SerializedMouseAction_ScrollLeft]) {
                ActiveUsbMouseReport->wheelY = -mouseScrollDistanceIntegerSum;
            }
            mouseScrollDistanceSum = mouseScrollDistanceFractionSum;
        }
    } else {
        mouseScrollDistanceSum = 0;
    }

    if (activeMouseStates[SerializedMouseAction_LeftClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Left;
    }
    if (activeMouseStates[SerializedMouseAction_MiddleClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Middle;
    }
    if (activeMouseStates[SerializedMouseAction_RightClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Right;
    }

}

static layer_id_t previousLayer = LayerId_Base;
static uint8_t basicScancodeIndex = 0;
static uint8_t mediaScancodeIndex = 0;
static uint8_t systemScancodeIndex = 0;
key_state_t *doubleTapSwitchLayerKey;
uint32_t doubleTapSwitchLayerStartTime;

void applyKeyAction(key_state_t *keyState, key_action_t *action)
{
    if (keyState->suppressed) {
        return;
    }

    if (doubleTapSwitchLayerKey && doubleTapSwitchLayerKey != keyState && !keyState->previous) {
        doubleTapSwitchLayerKey = NULL;
    }

    switch (action->type) {
        case KeyActionType_Keystroke:
            ActiveUsbBasicKeyboardReport->modifiers |= action->keystroke.modifiers;

            switch (action->keystroke.keystrokeType) {
                case KeystrokeType_Basic:
                    if (basicScancodeIndex >= USB_BASIC_KEYBOARD_MAX_KEYS || action->keystroke.scancode == 0) {
                        break;
                    }
                    ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = action->keystroke.scancode;
                    break;
                case KeystrokeType_Media:
                    if (mediaScancodeIndex >= USB_MEDIA_KEYBOARD_MAX_KEYS) {
                        break;
                    }
                    ActiveUsbMediaKeyboardReport->scancodes[mediaScancodeIndex++] = action->keystroke.scancode;
                    break;
                case KeystrokeType_System:
                    if (systemScancodeIndex >= USB_SYSTEM_KEYBOARD_MAX_KEYS) {
                        break;
                    }
                    ActiveUsbSystemKeyboardReport->scancodes[systemScancodeIndex++] = action->keystroke.scancode;
                    break;
            }
            break;
        case KeyActionType_Mouse:
            activeMouseStates[action->mouseAction] = true;
            break;
        case KeyActionType_SwitchLayer:
            if (!keyState->previous && previousLayer == LayerId_Base && action->switchLayer.mode == SwitchLayerMode_HoldAndDoubleTapToggle) {
                if (doubleTapSwitchLayerKey) {
                    if (Timer_GetElapsedTime(&doubleTapSwitchLayerStartTime) < doubleTapSwitchLayerTimeout) {
                        ToggledLayer = action->switchLayer.layer;
                    }
                    doubleTapSwitchLayerKey = NULL;
                } else {
                    doubleTapSwitchLayerKey = keyState;
                    doubleTapSwitchLayerStartTime = CurrentTime;
                }
            }
            break;
        case KeyActionType_SwitchKeymap:
            if (!keyState->previous) {
                SwitchKeymap(action->switchKeymap.keymapId);
            }
            break;
    }
}

static uint8_t secondaryRoleState = SecondaryRoleState_Released;
static uint8_t secondaryRoleSlotId;
static uint8_t secondaryRoleKeyId;
static secondary_role_t secondaryRole;

void updateActiveUsbReports(void)
{
    memset(activeMouseStates, 0, ACTIVE_MOUSE_STATES_COUNT);

    static uint8_t previousModifiers = 0;
    elapsedTime = Timer_GetElapsedTime(&UsbReportUpdateTime);

    basicScancodeIndex = 0;
    mediaScancodeIndex = 0;
    systemScancodeIndex = 0;

    for (uint8_t keyId=0; keyId < RIGHT_KEY_MATRIX_KEY_COUNT; keyId++) {
        KeyStates[SlotId_RightKeyboardHalf][keyId].current = RightKeyMatrix.keyStates[keyId];
    }

    layer_id_t activeLayer = LayerId_Base;
    if (secondaryRoleState == SecondaryRoleState_Triggered && IS_SECONDARY_ROLE_LAYER_SWITCHER(secondaryRole)) {
        activeLayer = SECONDARY_ROLE_LAYER_TO_LAYER_ID(secondaryRole);
    }
    if (activeLayer == LayerId_Base) {
        activeLayer = GetActiveLayer();
    }
    bool suppressKeys = previousLayer != LayerId_Base && activeLayer == LayerId_Base;
    LedDisplay_SetLayer(activeLayer);

    if (MacroPlaying) {
        Macros_ContinueMacro();
        memcpy(&ActiveUsbMouseReport, &MacroMouseReport, sizeof MacroMouseReport);
        memcpy(&ActiveUsbBasicKeyboardReport, &MacroBasicKeyboardReport, sizeof MacroBasicKeyboardReport);
        memcpy(&ActiveUsbMediaKeyboardReport, &MacroMediaKeyboardReport, sizeof MacroMediaKeyboardReport);
        memcpy(&ActiveUsbSystemKeyboardReport, &MacroSystemKeyboardReport, sizeof MacroSystemKeyboardReport);
        return;
    }

    for (uint8_t slotId=0; slotId<SLOT_COUNT; slotId++) {
        for (uint8_t keyId=0; keyId<MAX_KEY_COUNT_PER_MODULE; keyId++) {
            key_state_t *keyState = &KeyStates[slotId][keyId];
            key_action_t *action = &CurrentKeymap[activeLayer][slotId][keyId];


            if (keyState->debounceCounter < KEY_DEBOUNCER_TIMEOUT_MSEC) {
                keyState->current = keyState->previous;
            } else if (!keyState->previous && keyState->current) {
                keyState->debounceCounter = 0;
            }

            if (keyState->current) {
                if (suppressKeys) {
                    keyState->suppressed = true;
                }

                if (action->type == KeyActionType_Keystroke && action->keystroke.secondaryRole) {
                    // Press released secondary role key.
                    if (!keyState->previous && action->type == KeyActionType_Keystroke && action->keystroke.secondaryRole && secondaryRoleState == SecondaryRoleState_Released) {
                        secondaryRoleState = SecondaryRoleState_Pressed;
                        secondaryRoleSlotId = slotId;
                        secondaryRoleKeyId = keyId;
                        secondaryRole = action->keystroke.secondaryRole;
                        keyState->suppressed = true;
                    }
                } else {
                    // Trigger secondary role.
                    if (!keyState->previous && secondaryRoleState == SecondaryRoleState_Pressed) {
                        secondaryRoleState = SecondaryRoleState_Triggered;
                    } else {
                        applyKeyAction(keyState, action);
                    }
                }
            } else {
                if (keyState->suppressed) {
                    keyState->suppressed = false;
                }

                // Release secondary role key.
                if (keyState->previous && secondaryRoleSlotId == slotId && secondaryRoleKeyId == keyId) {
                    // Trigger primary role.
                    if (secondaryRoleState == SecondaryRoleState_Pressed) {
                        applyKeyAction(keyState, action);
                    }
                    secondaryRoleState = SecondaryRoleState_Released;
                }
            }

            keyState->previous = keyState->current;
        }
    }

    processMouseActions();

    // When a layer switcher key gets pressed along with another key that produces some modifiers
    // and the accomanying key gets released then keep the related modifiers active a long as the
    // layer switcher key stays pressed.  Useful for Alt+Tab keymappings and the like.
    if (activeLayer != LayerId_Base && activeLayer == PreviousHeldLayer && basicScancodeIndex == 0) {
        ActiveUsbBasicKeyboardReport->modifiers |= previousModifiers;
    }

    if (secondaryRoleState == SecondaryRoleState_Triggered && IS_SECONDARY_ROLE_MODIFIER(secondaryRole)) {
        ActiveUsbBasicKeyboardReport->modifiers |= SECONDARY_ROLE_MODIFIER_TO_HID_MODIFIER(secondaryRole);
    }

    previousModifiers = ActiveUsbBasicKeyboardReport->modifiers;
    previousLayer = activeLayer;
}

bool UsbBasicKeyboardReportEverSent = false;
bool UsbMediaKeyboardReportEverSent = false;
bool UsbSystemKeyboardReportEverSent = false;
bool UsbMouseReportEverSentEverSent = false;

void UpdateUsbReports(void)
{
    if (IsUsbBasicKeyboardReportSent) {
        UsbBasicKeyboardReportEverSent = true;
    }
    if (IsUsbMediaKeyboardReportSent) {
        UsbMediaKeyboardReportEverSent = true;
    }
    if (IsUsbSystemKeyboardReportSent) {
        UsbSystemKeyboardReportEverSent = true;
    }
    if (IsUsbMouseReportSent) {
        UsbMouseReportEverSentEverSent = true;
    }

    bool areUsbReportsSent = true;
    if (UsbBasicKeyboardReportEverSent) {
        areUsbReportsSent &= IsUsbBasicKeyboardReportSent;
    }
    if (UsbMediaKeyboardReportEverSent) {
        areUsbReportsSent &= IsUsbMediaKeyboardReportSent;
    }
    if (UsbSystemKeyboardReportEverSent) {
        areUsbReportsSent &= IsUsbSystemKeyboardReportSent;
    }
    if (UsbMouseReportEverSentEverSent) {
        areUsbReportsSent &= IsUsbMouseReportSent;
    }
    if (!areUsbReportsSent) {
        return;
    }

    ResetActiveUsbBasicKeyboardReport();
    ResetActiveUsbMediaKeyboardReport();
    ResetActiveUsbSystemKeyboardReport();
    ResetActiveUsbMouseReport();

    updateActiveUsbReports();

    SwitchActiveUsbBasicKeyboardReport();
    SwitchActiveUsbMediaKeyboardReport();
    SwitchActiveUsbSystemKeyboardReport();
    SwitchActiveUsbMouseReport();

    IsUsbBasicKeyboardReportSent = false;
    IsUsbMediaKeyboardReportSent = false;
    IsUsbSystemKeyboardReportSent = false;
    IsUsbMouseReportSent = false;
}
