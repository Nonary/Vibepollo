Const HKEY_CURRENT_USER = &H80000001
Const HKEY_LOCAL_MACHINE = &H80000002

Function RemoveConflictingProducts()
    On Error Resume Next

    Dim shell, reg, processed
    Set shell = CreateObject("WScript.Shell")
    Set reg = GetObject("winmgmts:\\.\root\default:StdRegProv")
    Set processed = CreateObject("Scripting.Dictionary")

    If Err.Number <> 0 Then
        LogMessage "RemoveConflictingProducts: failed to initialize objects: " & Err.Description
        RemoveConflictingProducts = 3
        Exit Function
    End If

    Dim hives(1), roots(1)
    hives(0) = HKEY_LOCAL_MACHINE
    hives(1) = HKEY_CURRENT_USER
    roots(0) = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    roots(1) = "SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"

    Dim hive, root, enumResult, subKeys, i
    Dim foundAny
    foundAny = False

    For Each hive In hives
        For Each root In roots
            enumResult = reg.EnumKey(hive, root, subKeys)
            If enumResult = 0 And IsArray(subKeys) Then
                For i = 0 To UBound(subKeys)
                    Dim subKeyName, fullPath, displayName
                    subKeyName = CStr(subKeys(i))
                    fullPath = root & "\" & subKeyName
                    displayName = ReadStringValue(reg, hive, fullPath, "DisplayName")

                    If IsTargetProduct(displayName) Then
                        foundAny = True
                        If Not UninstallConflict(reg, shell, hive, fullPath, subKeyName, displayName, processed) Then
                            RemoveConflictingProducts = 3
                            Exit Function
                        End If
                    End If
                Next
            End If
        Next
    Next

    If Not foundAny Then
        LogMessage "RemoveConflictingProducts: no conflicting products detected."
    End If

    RemoveConflictingProducts = 1
End Function

Private Function UninstallConflict(reg, shell, hive, fullPath, subKeyName, displayName, processed)
    UninstallConflict = False

    Dim windowsInstaller, quietCmd, uninstallCmd, commandToRun, productCode
    windowsInstaller = ReadDwordValue(reg, hive, fullPath, "WindowsInstaller")
    quietCmd = Trim(ReadStringValue(reg, hive, fullPath, "QuietUninstallString"))
    uninstallCmd = Trim(ReadStringValue(reg, hive, fullPath, "UninstallString"))
    productCode = Trim(subKeyName)

    If windowsInstaller = 1 And LooksLikeProductCode(productCode) Then
        commandToRun = BuildMsiUninstallCommand(shell, productCode)
    Else
        If Len(quietCmd) > 0 Then
            commandToRun = quietCmd
        Else
            commandToRun = uninstallCmd
        End If

        If Len(commandToRun) = 0 Then
            LogMessage "RemoveConflictingProducts: no uninstall command found for " & displayName
            Exit Function
        End If

        If Len(quietCmd) = 0 And (Not CommandHasQuietSwitch(commandToRun)) And (Not CommandTargetsMsiexec(commandToRun)) Then
            commandToRun = commandToRun & " /S"
        End If
    End If

    Dim dedupeKey
    dedupeKey = UCase(productCode & "|" & commandToRun)
    If processed.Exists(dedupeKey) Then
        UninstallConflict = True
        Exit Function
    End If
    processed.Add dedupeKey, True

    LogMessage "RemoveConflictingProducts: uninstalling " & displayName & " using: " & commandToRun

    Dim exitCode
    exitCode = shell.Run(commandToRun, 0, True)
    LogMessage "RemoveConflictingProducts: uninstall exit code for " & displayName & ": " & CStr(exitCode)

    If Not IsAcceptedExitCode(exitCode) Then
        LogMessage "RemoveConflictingProducts: uninstall failed for " & displayName
        Exit Function
    End If

    UninstallConflict = True
End Function

Private Function IsTargetProduct(displayName)
    Dim nameUpper
    nameUpper = UCase(Trim(displayName))
    IsTargetProduct = (Left(nameUpper, 8) = "SUNSHINE") _
        Or (Left(nameUpper, 6) = "APOLLO") _
        Or (Left(nameUpper, 9) = "VIBESHINE")
End Function

Private Function BuildMsiUninstallCommand(shell, productCode)
    Dim msiPath
    msiPath = shell.ExpandEnvironmentStrings("%WINDIR%\System32\msiexec.exe")
    BuildMsiUninstallCommand = Chr(34) & msiPath & Chr(34) _
        & " /x " & productCode _
        & " /qn /norestart REBOOT=ReallySuppress SUPPRESSMSGBOXES=1"
End Function

Private Function LooksLikeProductCode(value)
    Dim trimmed
    trimmed = Trim(value)
    LooksLikeProductCode = (Len(trimmed) = 38) _
        And (Left(trimmed, 1) = "{") _
        And (Right(trimmed, 1) = "}")
End Function

Private Function CommandHasQuietSwitch(commandLine)
    Dim upper
    upper = " " & UCase(commandLine) & " "
    CommandHasQuietSwitch = (InStr(upper, " /S ") > 0) _
        Or (InStr(upper, " /SILENT ") > 0) _
        Or (InStr(upper, " /VERYSILENT ") > 0) _
        Or (InStr(upper, " /QUIET ") > 0) _
        Or (InStr(upper, " /QN ") > 0)
End Function

Private Function CommandTargetsMsiexec(commandLine)
    Dim executable
    executable = LCase(GetExecutablePath(commandLine))
    If Right(executable, 12) = "\msiexec.exe" Then
        CommandTargetsMsiexec = True
        Exit Function
    End If
    If executable = "msiexec.exe" Or executable = "msiexec" Then
        CommandTargetsMsiexec = True
        Exit Function
    End If
    CommandTargetsMsiexec = False
End Function

Private Function GetExecutablePath(commandLine)
    Dim value, closingQuote, firstSpace
    value = Trim(commandLine)
    If Len(value) = 0 Then
        GetExecutablePath = ""
        Exit Function
    End If

    If Left(value, 1) = Chr(34) Then
        closingQuote = InStr(2, value, Chr(34))
        If closingQuote > 2 Then
            GetExecutablePath = Mid(value, 2, closingQuote - 2)
            Exit Function
        End If
    End If

    firstSpace = InStr(value, " ")
    If firstSpace <= 0 Then
        GetExecutablePath = value
    Else
        GetExecutablePath = Left(value, firstSpace - 1)
    End If
End Function

Private Function IsAcceptedExitCode(code)
    IsAcceptedExitCode = (code = 0) Or (code = 3010) Or (code = 1605)
End Function

Private Function ReadStringValue(reg, hive, path, valueName)
    On Error Resume Next
    Dim value, rc
    value = ""
    rc = reg.GetStringValue(hive, path, valueName, value)
    If rc <> 0 Or IsNull(value) Then
        value = ""
        rc = reg.GetExpandedStringValue(hive, path, valueName, value)
    End If
    If IsNull(value) Then
        value = ""
    End If
    ReadStringValue = CStr(value)
End Function

Private Function ReadDwordValue(reg, hive, path, valueName)
    On Error Resume Next
    Dim value, rc
    value = 0
    rc = reg.GetDWORDValue(hive, path, valueName, value)
    If rc <> 0 Or IsNull(value) Then
        value = 0
    End If
    ReadDwordValue = CLng(value)
End Function

Private Sub LogMessage(message)
    On Error Resume Next
    Session.Log message
End Sub
