Function AskRemoveSudoVda()
    Dim resp
resp = MsgBox("Do you want to remove the SudoVDA Virtual Display Driver?", vbQuestion + vbYesNo + vbDefaultButton2 + vbSystemModal + vbMsgBoxSetForeground, "Apollo")
    If resp = vbYes Then
        Session.Property("REMOVESUDOVDA") = "1"
    End If
End Function
