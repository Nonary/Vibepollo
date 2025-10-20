Function AskRemoveGamepad()
    Dim resp
    resp = MsgBox("Do you want to remove the virtual gamepad driver?", vbQuestion + vbYesNo + vbDefaultButton2 + vbSystemModal + vbMsgBoxSetForeground, "Vibepollo")
    If resp = vbYes Then
        Session.Property("REMOVEGAMEPAD") = "1"
    End If
End Function

