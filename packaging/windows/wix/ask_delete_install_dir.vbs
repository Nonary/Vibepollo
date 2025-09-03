Function AskDeleteInstallDir()
    Dim resp
    resp = MsgBox("Do you want to remove the installation directory (this includes the configuration, cover images, and settings)?", vbQuestion + vbYesNo + vbDefaultButton2, "Sunshine")
    If resp = vbYes Then
        Session.Property("DELETEINSTALLDIR") = "1"
    End If
End Function

