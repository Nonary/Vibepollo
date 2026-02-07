Function AskUninstallLegacySunshine()
    Dim resp, shell, uninstallPath, fso, result
    
    ' Get the uninstall path from MSI property
    uninstallPath = Session.Property("LEGACY_SUNSHINE_UNINSTALL")
    
    ' Remove quotes if present
    If Left(uninstallPath, 1) = """" Then
        uninstallPath = Mid(uninstallPath, 2, Len(uninstallPath) - 2)
    End If
    
    ' Check if uninstaller actually exists
    Set fso = CreateObject("Scripting.FileSystemObject")
    If Not fso.FileExists(uninstallPath) Then
        ' Uninstaller not found - registry entry is stale, just continue
        Session.Property("UNINSTALL_LEGACY_SUNSHINE") = "0"
        Exit Function
    End If
    
    ' Ask user if they want to uninstall legacy Sunshine
    resp = MsgBox("A legacy Sunshine installation was detected on this system." & vbCrLf & vbCrLf & _
                  "It must be removed before Vibeshine can be installed. " & _
                  "Your existing configuration will be preserved." & vbCrLf & vbCrLf & _
                  "Click OK to uninstall legacy Sunshine and continue, or Cancel to abort.", _
                  vbQuestion + vbOKCancel + vbDefaultButton1 + vbMsgBoxSetForeground, _
                  "Vibeshine Installer")
    
    If resp = vbOK Then
        ' Launch the uninstaller with elevation
        Set shell = CreateObject("Shell.Application")
        On Error Resume Next
        shell.ShellExecute uninstallPath, "/S", "", "runas", 0
        result = Err.Number
        On Error Goto 0
        
        If result = 0 Then
            Session.Property("UNINSTALL_LEGACY_SUNSHINE") = "1"
        Else
            ' Failed to launch - user may have cancelled UAC
            Session.Property("USER_CANCELLED_LEGACY_UNINSTALL") = "1"
        End If
    Else
        ' User cancelled - set property to block installation
        Session.Property("USER_CANCELLED_LEGACY_UNINSTALL") = "1"
    End If
End Function
