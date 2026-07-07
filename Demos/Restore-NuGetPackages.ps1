#requires -Version 5.1
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
	[ValidateNotNullOrEmpty()]
	[string] $Configuration = 'Debug',

	[ValidateSet('Win32', 'x64', 'ARM64')]
	[string] $Platform = 'x64',

	[switch] $Clean
)

$ErrorActionPreference = 'Stop'

$SrcPath = Split-Path -Parent $PSCommandPath
$PackagesPath = Join-Path $SrcPath 'packages'
$PackagesPathPrefix = "$PackagesPath\"

function Get-MSBuildPath {
	if ($env:MSBUILD_EXE_PATH -and (Test-Path -LiteralPath $env:MSBUILD_EXE_PATH -PathType Leaf)) {
		return $env:MSBUILD_EXE_PATH
	}

	$msbuildCommand = Get-Command 'msbuild.exe' -ErrorAction SilentlyContinue
	if ($msbuildCommand) {
		return $msbuildCommand.Source
	}

	$vswhereCandidates = @()
	if (${env:ProgramFiles(x86)}) {
		$vswhereCandidates += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	}
	if ($env:ProgramFiles) {
		$vswhereCandidates += Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
	}

	$vswhereCandidates =
		$vswhereCandidates |
		Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
		Select-Object -Unique

	foreach ($vswhere in $vswhereCandidates) {
		foreach ($msbuildPattern in @('MSBuild\**\Bin\amd64\MSBuild.exe', 'MSBuild\**\Bin\MSBuild.exe')) {
			$msbuildPaths =
				& $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find $msbuildPattern

			if ($LASTEXITCODE -ne 0) {
				continue
			}

			$msbuildPath =
				$msbuildPaths |
				Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
				Select-Object -First 1

			if ($msbuildPath) {
				return $msbuildPath
			}
		}
	}

	throw 'MSBuild.exe was not found. Install Visual Studio or Build Tools with the Desktop development with C++ workload, or run this from a Developer PowerShell where msbuild.exe is on PATH.'
}

if (-not (Test-Path -LiteralPath (Join-Path $SrcPath 'Directory.Packages.props') -PathType Leaf)) {
	throw "Directory.Packages.props was not found in '$SrcPath'. Run this script from the repository's src directory."
}

if ($Clean -and (Test-Path -LiteralPath $PackagesPath -PathType Container)) {
	if ($PSCmdlet.ShouldProcess($PackagesPath, 'Remove NuGet packages directory recursively')) {
		Remove-Item -LiteralPath $PackagesPath -Recurse -Force
	}
}

$projects =
	Get-ChildItem -LiteralPath $SrcPath -Filter '*.vcxproj' -Recurse -File |
	Where-Object { -not $_.FullName.StartsWith($PackagesPathPrefix, [System.StringComparison]::OrdinalIgnoreCase) } |
	Sort-Object FullName

if (-not $projects) {
	throw "No Visual C++ projects were found under '$SrcPath'."
}

$msbuildPath = Get-MSBuildPath
Write-Host "Using MSBuild: $msbuildPath"

foreach ($project in $projects) {
	Write-Host "Restoring $($project.FullName)"

	& $msbuildPath `
		$project.FullName `
		'/nologo' `
		'/t:Restore' `
		"/p:Configuration=$Configuration" `
		"/p:Platform=$Platform" `
		'/v:minimal'

	if ($LASTEXITCODE -ne 0) {
		throw "NuGet package restore failed for '$($project.FullName)'."
	}
}

Write-Host "NuGet package restore completed. Packages path: $PackagesPath"
