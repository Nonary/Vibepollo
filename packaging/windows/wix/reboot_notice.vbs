Option Explicit

Sub ShowRebootNotice
    Dim message
    message = "No reboot is needed when installing, upgrading, or uninstalling Vibeshine." & vbCrLf & vbCrLf & _
              "Vibeshine uses an MSI installer, which enables seamless in-place upgrades and unattended installs." & vbCrLf & _
              "Due to API limitations, the installer may still claim that a reboot is required. Removing that warning would force the installer to pause for over a minute while checking for file locks." & vbCrLf & vbCrLf & _
              "Please ignore any reboot prompts you see; the installation will finish without restarting."
    MsgBox message, vbInformation + vbSystemModal + vbMsgBoxSetForeground, "Reboot Notice"
End Sub
