#include "usb_protocol_handler.h"
#include "usb_commands/usb_command_get_variable.h"
#include "key_matrix.h"
#include "test_switches.h"
#include "usb_report_updater.h"

void UsbCommand_GetVariable(void)
{
    usb_variable_id_t variableId = GetUsbRxBufferUint8(1);

    switch (variableId) {
        case UsbVariable_TestSwitches:
            SetUsbTxBufferUint8(1, TestSwitches);
            break;
        case UsbVariable_TestUsbStack:
            SetUsbTxBufferUint8(1, TestUsbStack);
            break;
        case UsbVariable_DebounceTimePress:
            SetUsbTxBufferUint8(1, DebounceTimePress);
            break;
        case UsbVariable_DebounceTimeRelease:
            SetUsbTxBufferUint8(1, DebounceTimeRelease);
            break;
        case UsbVariable_UsbReportSemaphore:
            SetUsbTxBufferUint8(1, UsbReportUpdateSemaphore);
            break;
    }
}
