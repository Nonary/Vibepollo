Function WaitForApolloUninstall()
    Dim shell, regKey64, regKey32, apolloPresent, maxWaitSeconds, elapsedSeconds
    
    ' Silent uninstall was launched - wait for it to complete
    ' Poll the registry every second for up to 30 seconds
    maxWaitSeconds = 30
    elapsedSeconds = 0
    Set shell = CreateObject("WScript.Shell")
    apolloPresent = True
    
    Do While elapsedSeconds < maxWaitSeconds And apolloPresent
        ' Use ping as a sleep mechanism (MSI context doesn't have WScript.Sleep)
        shell.Run "ping 127.0.0.1 -n 2", 0, True
        elapsedSeconds = elapsedSeconds + 1
        
        ' Check if Apollo is still present
        apolloPresent = False
        
        ' Check 64-bit registry
        On Error Resume Next
        regKey64 = shell.RegRead("HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Apollo\DisplayName")
        If Err.Number = 0 And Len(regKey64) > 0 Then
            apolloPresent = True
        End If
        Err.Clear
        
        ' Check 32-bit registry
        regKey32 = shell.RegRead("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Apollo\DisplayName")
        If Err.Number = 0 And Len(regKey32) > 0 Then
            apolloPresent = True
        End If
        On Error Goto 0
    Loop
    
    ' Set property based on final state
    If apolloPresent Then
        Session.Property("APOLLO_STILL_PRESENT") = "1"
    Else
        Session.Property("APOLLO_STILL_PRESENT") = "0"
    End If
End Function

