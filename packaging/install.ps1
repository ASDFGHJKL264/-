param([string]$InstallDirectory, [switch]$NoShortcuts, [switch]$Quiet)
$ErrorActionPreference = 'Stop'
function Get-PayloadHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
try {
    $product = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'product.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($InstallDirectory)) { $InstallDirectory = $product.installDirectory }
    $target = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
    if ($target.Length -le [IO.Path]::GetPathRoot($target).Length) { throw 'An application directory is required.' }
    $payload = Join-Path $PSScriptRoot 'payload'
    $prefix = $target + '\'
    $active = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($active.Count) { throw 'Close the application before updating. No running process was stopped.' }
    # Validate every payload file before modifying an installed application.
    $manifest = Get-Content -LiteralPath (Join-Path $payload 'SHA256.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($entry in $manifest.PSObject.Properties) {
        $source = [IO.Path]::GetFullPath((Join-Path $payload $entry.Name))
        if (!$source.StartsWith($payload + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid payload path.' }
        if ((Get-PayloadHash $source) -ne $entry.Value) { throw ('Payload verification failed: ' + $entry.Name) }
    }
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    $backup = Join-Path $target ('_update-backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    $written = New-Object 'System.Collections.Generic.List[object]'
    try {
        foreach ($file in (Get-ChildItem -LiteralPath $payload -File -Recurse)) {
            $relative = $file.FullName.Substring($payload.Length + 1)
            # Packages contain examples only. Never overwrite local user data/configuration.
            if ($relative -match '^(data|logs|_update-backups)\\' -or $relative -eq 'config.ini') { throw 'Mutable application data must not be included in a package.' }
            $destination = [IO.Path]::GetFullPath((Join-Path $target $relative))
            if (!$destination.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid installation path.' }
            $previous = $null
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                $previous = Join-Path $backup $relative
                New-Item -ItemType Directory -Path (Split-Path $previous -Parent) -Force | Out-Null
                Copy-Item -LiteralPath $destination -Destination $previous -Force
            }
            New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
            $written.Add([pscustomobject]@{Destination=$destination; Previous=$previous})
            Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
        }
    } catch {
        # Restore replaced files; only remove newly created individual files within target.
        for ($i = $written.Count - 1; $i -ge 0; $i--) {
            $entry = $written[$i]
            if ($entry.Previous) { Copy-Item -LiteralPath $entry.Previous -Destination $entry.Destination -Force }
            elseif (Test-Path -LiteralPath $entry.Destination -PathType Leaf) { Remove-Item -LiteralPath $entry.Destination -Force }
        }
        throw
    }
    $example = Join-Path $target 'config.example.ini'
    $configuration = Join-Path $target 'config.ini'
    if ((Test-Path -LiteralPath $example) -and !(Test-Path -LiteralPath $configuration)) {
        Copy-Item -LiteralPath $example -Destination $configuration
    }
    if (!$NoShortcuts) {
        $desktop = [Environment]::GetFolderPath('Desktop')
        $shell = New-Object -ComObject WScript.Shell
        foreach ($app in $product.applications) {
            $link = $shell.CreateShortcut((Join-Path $desktop ($app.shortcut + '.lnk')))
            $link.TargetPath = Join-Path $target $app.executable
            $link.WorkingDirectory = $target
            $link.Description = $product.name + ' ' + $product.version
            $link.Save()
        }
    }
    Write-Output ('Installed: ' + $target)
    if (!$Quiet) { (New-Object -ComObject WScript.Shell).Popup(('Installed: ' + $target), 0, 'Installation complete', 64) | Out-Null }
} catch {
    if (!$Quiet) { (New-Object -ComObject WScript.Shell).Popup($_.Exception.Message, 0, 'Installation failed', 16) | Out-Null }
    Write-Error $_
    exit 1
}
