[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [ValidateRange(6, 60)]
    [int]$Frames = 8,
    [ValidateRange(1, 600)]
    [int]$MaxSeconds = 45,
    [ValidateRange(0.001, 1.0)]
    [double]$MinimumChangedPixelFraction = 0.01
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$rendererRoot =
    [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$harness = Join-Path $rendererRoot "tools\Invoke-Oot3dRendererDev.ps1"
$buildScript = Join-Path $rendererRoot `
    "..\..\tools\oot3d\build_fast_dev.ps1"
$buildDirectory = Join-Path $rendererRoot ".renderer-dev\build"
$foundationTests = Join-Path $buildDirectory `
    "three_ds_recomp_runtime\tests\oot3d_graphics_foundation_tests.exe"
$artifactRoot = Join-Path $rendererRoot ".renderer-dev\artifacts"
$diagnosticsPath = Join-Path $artifactRoot "vulkan-diagnostics.json"
$screenshotPath = Join-Path $artifactRoot "native-game.bmp"
Import-Module (Join-Path $rendererRoot `
    "tools\Oot3dFramebufferArtifact.psm1") -Force

$environmentNames = @(
    "OOT3D_GRAPHICS_AO",
    "OOT3D_GRAPHICS_AO_QUALITY",
    "OOT3D_GRAPHICS_AO_STRESS",
    "OOT3D_VULKAN_VALIDATION"
)
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] =
        [Environment]::GetEnvironmentVariable($name, "Process")
}

function Invoke-AoCase {
    param(
        [string]$Label,
        [bool]$Stress
    )
    $env:OOT3D_GRAPHICS_AO = "1"
    $env:OOT3D_GRAPHICS_AO_QUALITY = "1"
    $env:OOT3D_VULKAN_VALIDATION = "1"
    if ($Stress) {
        $env:OOT3D_GRAPHICS_AO_STRESS = "1"
    } else {
        Remove-Item Env:OOT3D_GRAPHICS_AO_STRESS `
            -ErrorAction SilentlyContinue
    }
    & $harness -SkipBuild -Frames $Frames -MaxSeconds $MaxSeconds |
        Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$Label GTAO runtime exited with $LASTEXITCODE"
    }
    $diagnostics =
        Get-Content -LiteralPath $diagnosticsPath -Raw |
        ConvertFrom-Json
    $diagnosticFrames = @($diagnostics.frames)
    if ($diagnosticFrames.Count -ne $Frames) {
        throw "$Label produced $($diagnosticFrames.Count)/$Frames frames"
    }
    $vulkanErrors = [uint64](
        $diagnosticFrames |
            Measure-Object -Property vulkan_validation_error_count -Maximum
    ).Maximum
    $nriErrors = [uint64](
        $diagnosticFrames |
            Measure-Object -Property nri_validation_error_count -Maximum
    ).Maximum
    $nativeDraws = [uint64](
        $diagnosticFrames |
            Measure-Object -Property native_pica_draw_count -Sum
    ).Sum
    $nriDraws = [uint64](
        $diagnosticFrames |
            Measure-Object -Property nri_pica_owned_draw_count -Sum
    ).Sum
    $aoPasses = [uint64](
        $diagnosticFrames |
            Measure-Object -Property ambient_occlusion_pass_count -Sum
    ).Sum
    $normalPasses = [uint64](
        $diagnosticFrames |
            Measure-Object -Property ambient_occlusion_normal_guide_pass_count -Sum
    ).Sum
    if ($vulkanErrors -ne 0U -or $nriErrors -ne 0U) {
        throw "$Label produced validation errors"
    }
    if ($nativeDraws -eq 0U -or $nativeDraws -ne $nriDraws) {
        throw "$Label broke native PICA/NRI draw parity"
    }
    if ($aoPasses -eq 0U -or $normalPasses -ne $aoPasses) {
        throw "$Label did not execute medium-quality GTAO"
    }
    $framebuffer = Read-Oot3dFramebufferBmp -Path $screenshotPath
    $metrics = Get-Oot3dFramebufferMetrics -Framebuffer $framebuffer
    if ($metrics.UniqueColors -lt 32 -or
        $metrics.ChannelRange -lt 24) {
        throw "$Label produced a collapsed framebuffer"
    }
    return [pscustomobject]@{
        Label = $Label
        Draws = $nativeDraws
        AmbientOcclusionPasses = $aoPasses
        Framebuffer = $framebuffer
    }
}

try {
    if (-not $SkipBuild) {
        & $buildScript -Target oot3d_graphics_foundation_tests `
            -BuildDirectory $buildDirectory -Parallel 2 | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to build AO control contract tests"
        }
        & $foundationTests `
            "--gtest_filter=Oot3dGraphicsSettingsPersistence.*:Oot3dAmbientOcclusionComposite.*" |
            Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "AO control contract tests failed"
        }
        & $buildScript -Target oot3d_native_game `
            -BuildDirectory $buildDirectory -Parallel 2 | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to build the native game"
        }
    }

    $baseline = Invoke-AoCase -Label "Default" -Stress $false
    $stress = Invoke-AoCase -Label "Stress" -Stress $true
    if ($baseline.Draws -ne $stress.Draws -or
        $baseline.AmbientOcclusionPasses -ne $stress.AmbientOcclusionPasses) {
        throw "AO controls changed draw or pass coverage"
    }
    $difference = Compare-Oot3dFramebufferBmp `
        -Reference $baseline.Framebuffer `
        -Candidate $stress.Framebuffer `
        -ChangedPixelThreshold 2
    if ($difference.ChangedPixelFraction -lt
        $MinimumChangedPixelFraction) {
        throw "Advanced AO controls were not visibly effective: " +
            "$($difference.ChangedPixelFraction)"
    }

    [pscustomobject]@{
        Result = "PASS"
        FramesPerCase = $Frames
        DrawsPerCase = $baseline.Draws
        AmbientOcclusionPassesPerCase = $baseline.AmbientOcclusionPasses
        ChangedPixelFraction = $difference.ChangedPixelFraction
        MeanAbsoluteChannelDelta =
            $difference.MeanAbsoluteChannelDelta
        MaximumChannelDelta = $difference.MaximumChannelDelta
    } | Format-List
} finally {
    foreach ($name in $environmentNames) {
        $previous = $previousEnvironment[$name]
        if ($null -eq $previous) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable(
                $name, [string]$previous, "Process")
        }
    }
}
