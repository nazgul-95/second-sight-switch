param([switch]$LiftOnly)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot '../..'))
$toolkit = Join-Path $workspaceRoot 'external_versions/mksm-switch/xboxrecomp'
$python = Join-Path $workspaceRoot 'external_versions/xboxrecomp/.venv/Scripts/python.exe'
$xbe = Join-Path $projectRoot 'game_files/game_ntsc.xbe'
$analysis = Join-Path $projectRoot 'analysis_boot'
$generated = Join-Path $projectRoot 'generated_boot'
if ((Get-FileHash -LiteralPath $xbe -Algorithm SHA256).Hash -ne '226EBC4834FBD2F8F7C7E657FB4B1C8B8A7F53105D9189E6615BCA1F4983C83E') {
    throw 'Unsupported XBE revision; address seeds and overrides require the pinned NTSC image.'
}
Push-Location $toolkit
try {
    if (-not $LiftOnly) {
    & $python -m tools.disasm $xbe --force -o "$analysis/disasm" --seed-functions "$projectRoot/config/seed_functions.json"
    if ($LASTEXITCODE) { throw 'Disassembly failed' }
    & $python -m tools.func_id $xbe --functions "$analysis/disasm/functions.json" --strings "$analysis/disasm/strings.json" --xrefs "$analysis/disasm/xrefs.json" -o "$analysis/func_id"
    if ($LASTEXITCODE) { throw 'Function identification failed' }
    & $python -m tools.abi_analysis $xbe --disasm-dir "$analysis/disasm" --func-id-dir "$analysis/func_id" --output-dir "$analysis/abi"
    if ($LASTEXITCODE) { throw 'ABI analysis failed' }
    }
    & $python "$projectRoot/repair_analysis.py"
    if ($LASTEXITCODE) { throw 'Analysis repair evidence changed' }
    # Generator preserves unchanged output files for incremental builds.
    & $python -m tools.recomp $xbe --game-name 'Second Sight' --all --split 250 --disasm-dir "$analysis/disasm" --functions "$analysis/functions_repaired.json" --func-id-dir "$analysis/func_id" --abi-dir "$analysis/abi" --output-dir "$analysis/recomp" --gen-dir $generated --exclude-manual "$projectRoot/src/recomp_manual.c" --coalesce-functions "$projectRoot/config/function_bounds.json" --mmio-sections DSOUND,XPP,D3D,XGRPH,XMV
    if ($LASTEXITCODE) { throw 'Recompilation failed' }
    & $python "$projectRoot/patch_audio_dsp_ack.py" $generated
    if ($LASTEXITCODE) { throw 'Second Sight DSP acknowledgement patch failed' }
    # Preserve the donor's Switch/ABI glue while applying the reviewed
    # upstream per-edge flag conditions to five original title functions.
    & $python "$projectRoot/tools/apply_upstream_flag_joins.py" --apply
    if ($LASTEXITCODE) { throw 'Upstream title flag-join transfer failed' }
    & $python "$projectRoot/tools/patch_flow_trace_gate.py" --apply
    if ($LASTEXITCODE) { throw 'Bring-up trace gate patch failed' }
    & $python "$projectRoot/tools/patch_frame_profile.py" --apply
    if ($LASTEXITCODE) { throw 'Frame profiling boundary patch failed' }
    & $python "$projectRoot/tools/patch_spin_progress.py"
    if ($LASTEXITCODE) { throw 'Hardware progress wait hook patch failed' }
    & $python "$projectRoot/tools/patch_keyboard_trace.py"
    if ($LASTEXITCODE) { throw 'Keyboard observation hook patch failed' }
    & $python "$projectRoot/tests/check_boot_inventory.py" --generated $generated
    if ($LASTEXITCODE) { throw 'Generated boot inventory failed validation' }
} finally {
    Pop-Location
}

