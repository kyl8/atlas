# Dot-source this file from the PowerShell profile to enable shell navigation.
function global:atlas {
    $atlasArguments = @($args)
    $atlasExecutable = @(Get-Command atlas.exe -CommandType Application -ErrorAction Stop)[0].Source

    $atlasNavigationFlags = @('-G', '-gs', '--go-session')
    $atlasNavigationFlag = @($atlasArguments | Where-Object { $atlasNavigationFlags -ccontains $_ })
    if ($atlasArguments.Count -eq 0 -or @('project', 'p') -cnotcontains $atlasArguments[0] -or
        $atlasNavigationFlag.Count -eq 0 -or $atlasArguments -ccontains '--help') {
        & $atlasExecutable @atlasArguments
        $global:LASTEXITCODE = $LASTEXITCODE
        return
    }

    $atlasProject = $null
    if ($atlasArguments.Count -eq 3) {
        if ($atlasNavigationFlags -ccontains $atlasArguments[2]) { $atlasProject = $atlasArguments[1] }
        elseif ($atlasNavigationFlags -ccontains $atlasArguments[1]) { $atlasProject = $atlasArguments[2] }
    }
    elseif ($atlasArguments.Count -eq 4 -and $atlasArguments[1] -ceq 'open' -and
            $atlasNavigationFlags -ccontains $atlasArguments[3]) {
        $atlasProject = $atlasArguments[2]
    }
    if (-not $atlasProject -or $atlasProject.StartsWith('-')) {
        $global:LASTEXITCODE = 2
        Write-Error 'Usage: atlas project <name> -gs' -ErrorAction Continue
        return
    }

    # Windows PowerShell 5.1 otherwise decodes redirected native output with the OEM code page.
    $atlasPreviousEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
        $atlasPath = @(& $atlasExecutable --plain project path -- $atlasProject)
        $atlasExitCode = $LASTEXITCODE
    }
    finally {
        [Console]::OutputEncoding = $atlasPreviousEncoding
    }
    $global:LASTEXITCODE = $atlasExitCode
    if ($atlasExitCode -ne 0) { return }
    if ($atlasPath.Count -ne 1 -or [string]::IsNullOrWhiteSpace($atlasPath[0]) -or
        -not [System.IO.Path]::IsPathRooted($atlasPath[0])) {
        $global:LASTEXITCODE = 1
        Write-Error 'Atlas did not return a single absolute project path.' -ErrorAction Continue
        return
    }
    try {
        if (-not [System.IO.Directory]::Exists($atlasPath[0])) {
            throw "Project directory is unavailable: $($atlasPath[0])"
        }
        Set-Location -LiteralPath $atlasPath[0] -ErrorAction Stop
        $global:LASTEXITCODE = 0
    }
    catch {
        $global:LASTEXITCODE = 1
        Write-Error $_ -ErrorAction Continue
    }
}
