Function AskRemoveSudoVda()
    Dim resp
    resp = MsgBox("Would you like to remove the Virtual Display Driver (SudoVDA)?" & vbCrLf & vbCrLf & _
                  "Yes - Remove the driver. Choose this if you no longer need virtual monitors." & vbCrLf & _
                  "No  - Keep the driver installed for use with other applications.", _
                  vbQuestion + vbYesNo + vbDefaultButton2 + vbSystemModal + vbMsgBoxSetForeground, _
                  "Vibeshine")
    If resp = vbYes Then
        Session.Property("REMOVEVIRTUALDISPLAYDRIVER") = "1"
    End If
End Function
