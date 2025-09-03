Function AskRemoveGamepad()
    Dim resp
    resp = MsgBox("Do you want to remove Virtual Gamepad?", vbQuestion + vbYesNo + vbDefaultButton2, "Sunshine")
    If resp = vbYes Then
        Session.Property("REMOVEGAMEPAD") = "1"
    End If
End Function

