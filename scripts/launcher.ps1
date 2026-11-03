# The one way to start Operating System on this PC: a small window to choose how (your disk or a fresh one, the
# AI saved on it or Claude on this PC, a design to try, full screen), remembered for next time; Enter starts.
# It then runs os-win.ps1 with those choices. Run from "Operating System.cmd" (powershell -STA).
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$root = Split-Path -Parent $PSScriptRoot
$stateDir = Join-Path $env:LOCALAPPDATA "Operating System"
$stateFile = Join-Path $stateDir "launcher.json"
$last = @{ fresh = $false; claude = $false; model = "sonnet"; design = ""; fullscreen = $false; expressive = $false }
if (Test-Path $stateFile) {
    try {
        $j = Get-Content $stateFile -Raw | ConvertFrom-Json
        foreach ($k in @($last.Keys)) { if ($null -ne $j.$k) { $last[$k] = $j.$k } }
    } catch {}
}

# the designs that can be tried: the starter designs, and the hand-made ones in tests\designs
$designs = [ordered]@{ "As it is" = "" }
Get-ChildItem (Join-Path $root "userland\rootfs\usr\share\os\themes\*.xml") -ErrorAction SilentlyContinue | Sort-Object Name |
    ForEach-Object { $designs[(Get-Culture).TextInfo.ToTitleCase($_.BaseName)] = $_.FullName }
Get-ChildItem (Join-Path $root "tests\designs\*.xml") -ErrorAction SilentlyContinue | Sort-Object Name |
    ForEach-Object { $n = $_.BaseName.ToUpper(); if (-not $designs.Contains($n)) { $designs[$n] = $_.FullName } }

$font = New-Object System.Drawing.Font("Segoe UI", 10)
$bold = New-Object System.Drawing.Font("Segoe UI Semibold", 10)
$form = New-Object System.Windows.Forms.Form
$form.Text = "Operating System"
$form.Font = $font
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.StartPosition = "CenterScreen"
$form.ClientSize = New-Object System.Drawing.Size(460, 410)
$form.AutoScaleMode = "Dpi"

function Label($text, $x, $y, $f = $font) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Font = $f; $l.AutoSize = $true
    $l.Location = New-Object System.Drawing.Point($x, $y)
    $form.Controls.Add($l); $l
}
function Radio($text, $x, $y, $group) {
    $r = New-Object System.Windows.Forms.RadioButton
    $r.Text = $text; $r.AutoSize = $true
    $r.Location = New-Object System.Drawing.Point($x, $y)
    $group.Controls.Add($r); $r
}

[void](Label "Disk" 20 16 $bold)
$diskBox = New-Object System.Windows.Forms.Panel
$diskBox.Location = New-Object System.Drawing.Point(20, 38); $diskBox.Size = New-Object System.Drawing.Size(400, 52)
$form.Controls.Add($diskBox)
$mine = Radio "Your disk" 0 0 $diskBox
$fresh = Radio "A fresh disk: a new computer, from the first screen" 0 26 $diskBox
$fresh.Checked = [bool]$last.fresh; $mine.Checked = -not $fresh.Checked

$diskNote = Label "Your own disk is not touched." 38 92
$diskNote.ForeColor = [System.Drawing.SystemColors]::GrayText
$diskNote.Visible = $fresh.Checked
$fresh.Add_CheckedChanged({ $diskNote.Visible = $fresh.Checked })
[void](Label "AI" 20 120 $bold)
$aiBox = New-Object System.Windows.Forms.Panel
$aiBox.Location = New-Object System.Drawing.Point(20, 142); $aiBox.Size = New-Object System.Drawing.Size(400, 52)
$form.Controls.Add($aiBox)
$saved = Radio "The one saved on the disk" 0 0 $aiBox
$claude = Radio "Claude on this PC" 0 26 $aiBox
$model = New-Object System.Windows.Forms.ComboBox
$model.DropDownStyle = "DropDownList"
[void]$model.Items.AddRange(@("opus", "sonnet", "haiku", "fable"))
$model.SelectedItem = if ($model.Items.Contains($last.model)) { $last.model } else { "sonnet" }
$model.Location = New-Object System.Drawing.Point(($claude.PreferredSize.Width + 10), 23); $model.Width = 100
$aiBox.Controls.Add($model)
$claude.Checked = [bool]$last.claude; $saved.Checked = -not $claude.Checked
$model.Enabled = $claude.Checked
$claude.Add_CheckedChanged({ $model.Enabled = $claude.Checked })

[void](Label "Design" 20 204 $bold)
$design = New-Object System.Windows.Forms.ComboBox
$design.DropDownStyle = "DropDownList"
foreach ($k in $designs.Keys) { [void]$design.Items.Add($k) }
$pick = ($designs.Keys | Where-Object { $designs[$_] -eq $last.design } | Select-Object -First 1)
$design.SelectedItem = if ($pick) { $pick } else { "As it is" }
$design.Location = New-Object System.Drawing.Point(20, 228); $design.Width = 180
$form.Controls.Add($design)
$designNote = Label "" 210 231
$designNote.ForeColor = [System.Drawing.SystemColors]::GrayText
$setNote = { $designNote.Text = if ($design.SelectedItem -eq "As it is") { "" } else { "for this time only, on a copy of the disk" } }
$design.Add_SelectedIndexChanged($setNote); & $setNote

$full = New-Object System.Windows.Forms.CheckBox
$full.Text = "Full screen (Ctrl+Alt+F leaves it, Ctrl+Alt+G frees the mouse)"
$full.AutoSize = $true
$full.Location = New-Object System.Drawing.Point(20, 274)
$full.Checked = [bool]$last.fullscreen
$form.Controls.Add($full)

$expr = New-Object System.Windows.Forms.CheckBox
$expr.Text = "Expressive voice, on this PC's graphics card (feeling and cloning)"
$expr.AutoSize = $true
$expr.Location = New-Object System.Drawing.Point(20, 300)
$expr.Checked = [bool]$last.expressive
$expr.Enabled = Test-Path (Join-Path $root "build\voice-gpu\venv\Scripts\python.exe")
$form.Controls.Add($expr)

$start = New-Object System.Windows.Forms.Button
$start.Text = "Start"; $start.Font = $bold
$start.Size = New-Object System.Drawing.Size(110, 34)
$start.Location = New-Object System.Drawing.Point(330, 356)
$start.DialogResult = "OK"
$form.Controls.Add($start)
$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = "Cancel"
$cancel.Size = New-Object System.Drawing.Size(90, 34)
$cancel.Location = New-Object System.Drawing.Point(230, 356)
$cancel.DialogResult = "Cancel"
$form.Controls.Add($cancel)
$form.AcceptButton = $start
$form.CancelButton = $cancel
$form.Add_Shown({ $start.Focus() })

if ($env:OS_LAUNCHER_SHOT) { # (for checking how it looks: the window drawn to a picture, nothing started)
    $form.StartPosition = "Manual"; $form.Location = New-Object System.Drawing.Point(-3000, -3000)
    $form.ShowInTaskbar = $false
    $form.Show(); [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 300
    [System.Windows.Forms.Application]::DoEvents()
    $bmp = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
    $form.DrawToBitmap($bmp, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
    $bmp.Save($env:OS_LAUNCHER_SHOT)
    exit 0
}
if ($form.ShowDialog() -ne "OK") { exit 0 }

$choice = @{ fresh = $fresh.Checked; claude = $claude.Checked; model = [string]$model.SelectedItem
             design = [string]$designs[[string]$design.SelectedItem]; fullscreen = $full.Checked
             expressive = $expr.Checked }
New-Item -ItemType Directory -Force $stateDir | Out-Null
$choice | ConvertTo-Json | Set-Content -Encoding utf8 $stateFile

$how = @{}
if ($choice.fresh) { $how.Fresh = $true }
if ($choice.claude) { $how.Claude = $true; $how.Model = $choice.model }
if ($choice.design) { $how.Facet = $choice.design }
if ($choice.fullscreen) { $how.Fullscreen = $true }
if ($choice.expressive) { $how.Expressive = $true }
& (Join-Path $PSScriptRoot "os-win.ps1") @how
