Function AskDeleteInstallDir()
    Dim resp
    ' Ensure the prompt is foreground and top-most so users see it
    resp = MsgBox("Do you want to remove the installation directory (this includes the configuration, cover images, and settings)?", vbQuestion + vbYesNo + vbDefaultButton2 + vbSystemModal + vbMsgBoxSetForeground, "Sunshine")
    If resp = vbYes Then
        Session.Property("DELETEINSTALLDIR") = "1"
    End If
End Function
