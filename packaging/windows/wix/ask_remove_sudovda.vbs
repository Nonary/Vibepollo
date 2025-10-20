Function AskRemoveSudoVda()
    Dim resp
resp = MsgBox("Do you want to remove the Virtual Display Driver (SudoVDA)?", vbQuestion + vbYesNo + vbDefaultButton2 + vbSystemModal + vbMsgBoxSetForeground, "Apollo")
    If resp = vbYes Then
        Session.Property("REMOVEVIRTUALDISPLAYDRIVER") = "1"
    End If
End Function
