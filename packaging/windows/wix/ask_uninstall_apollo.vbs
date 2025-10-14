Function AskUninstallApollo()
    Dim resp, shell, uninstallPath, fso, result
    
    ' Get the uninstall path from MSI property
    uninstallPath = Session.Property("APOLLO_UNINSTALL")
    
    ' Remove quotes if present
    If Left(uninstallPath, 1) = """" Then
        uninstallPath = Mid(uninstallPath, 2, Len(uninstallPath) - 2)
    End If
    
    ' Check if uninstaller actually exists
    Set fso = CreateObject("Scripting.FileSystemObject")
    If Not fso.FileExists(uninstallPath) Then
        ' Uninstaller not found - registry entry is stale, just continue
        Session.Property("UNINSTALL_APOLLO") = "0"
        Exit Function
    End If
    
    ' Ask user if they want to uninstall existing Apollo
    resp = MsgBox("Existing Apollo installation detected. Would you like to uninstall it now?" & vbCrLf & vbCrLf & _
                  "The Apollo installation will be uninstalled silently (keeping your configuration)." & vbCrLf & _
                  "Click OK to proceed with Vibeshine installation.", _
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
            Session.Property("UNINSTALL_APOLLO") = "1"
        Else
            ' Failed to launch - user may have cancelled UAC
            Session.Property("USER_CANCELLED_APOLLO_UNINSTALL") = "1"
        End If
    Else
        ' User cancelled - set property to block installation
        Session.Property("USER_CANCELLED_APOLLO_UNINSTALL") = "1"
    End If
End Function
