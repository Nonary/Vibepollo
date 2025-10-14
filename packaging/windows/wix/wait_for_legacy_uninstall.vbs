Function WaitForLegacyUninstall()
    Dim shell, regKey64, regKey32, legacyPresent, maxWaitSeconds, elapsedSeconds
    
    ' Silent uninstall was launched - wait for it to complete
    ' Poll the registry every second for up to 30 seconds
    maxWaitSeconds = 30
    elapsedSeconds = 0
    Set shell = CreateObject("WScript.Shell")
    legacyPresent = True
    
    Do While elapsedSeconds < maxWaitSeconds And legacyPresent
        ' Use ping as a sleep mechanism (MSI context doesn't have WScript.Sleep)
        shell.Run "ping 127.0.0.1 -n 2", 0, True
        elapsedSeconds = elapsedSeconds + 1
        
        ' Check if legacy Sunshine is still present
        legacyPresent = False
        
        ' Check 64-bit registry
        On Error Resume Next
        regKey64 = shell.RegRead("HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine\DisplayName")
        If Err.Number = 0 And Len(regKey64) > 0 Then
            legacyPresent = True
        End If
        Err.Clear
        
        ' Check 32-bit registry
        regKey32 = shell.RegRead("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine\DisplayName")
        If Err.Number = 0 And Len(regKey32) > 0 Then
            legacyPresent = True
        End If
        On Error Goto 0
    Loop
    
    ' Set property based on final state
    If legacyPresent Then
        Session.Property("LEGACY_SUNSHINE_STILL_PRESENT") = "1"
    Else
        Session.Property("LEGACY_SUNSHINE_STILL_PRESENT") = "0"
    End If
End Function

