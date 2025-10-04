Function AskRemoveGamepad()
    Dim resp
    ' Ensure the prompt is foreground and top-most so users see it
resp = MsgBox("Do you want to remove Virtual Gamepad?", vbQuestion + vbYesNo + vbDefaultButton2 + vbSystemModal + vbMsgBoxSetForeground, "Apollo")
    If resp = vbYes Then
        Session.Property("REMOVEGAMEPAD") = "1"
    End If
End Function
