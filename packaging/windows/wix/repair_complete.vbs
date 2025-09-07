' Displays a simple message after a successful Repair to inform the user
' that no reboot is required.

Function ShowRepairComplete()
    Dim msg
    msg = "Repair completed successfully. No reboot is required."
    MsgBox msg, vbInformation + vbOKOnly, "Sunshine Repair"
    ShowRepairComplete = 0
End Function

