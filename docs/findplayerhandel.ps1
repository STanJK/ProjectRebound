# Path to SDK folder
$SDKPath = ".\SDK"

# Output files
$WeaponsOut   = "weapons.txt"
$RolesOut     = "roles.txt"
$InventoryOut = "inventory.txt"

# Clear old files
"" | Out-File $WeaponsOut
"" | Out-File $RolesOut
"" | Out-File $InventoryOut

# Category 1 — Weapon-related
$WeaponTerms = @(
    "PBWeapon",
    "Weapon",
    "Inventory",      # weapons stored here
    "ApplyPart",
    "WeaponPart",
    "Attachment"
)

# Category 2 — Role-related
$RoleTerms = @(
    "PBRole",
    "Role",
    "EPBRole",
    "SelectRole",
    "ServerConfirmRoleSelection"
)

# Category 3 — Inventory / Loadout-related
$InventoryTerms = @(
    "Inventory",
    "Loadout",
    "Equipment",
    "Pylon",
    "Utility",
    "Module",
    "Gadget"
)

Write-Host "Searching SDK for loadout categories..." -ForegroundColor Cyan

# Helper function to search and write results
function Search-And-Write {
    param(
        [string[]]$Terms,
        [string]$OutFile
    )

    Get-ChildItem -Path $SDKPath -Recurse -Include *.hpp, *.cpp | ForEach-Object {
        $file = $_.FullName
        $lines = Get-Content $file

        for ($i = 0; $i -lt $lines.Length; $i++) {
            foreach ($term in $Terms) {
                if ($lines[$i] -match "\b$term\b") {
                    Add-Content $OutFile "`nFile: $file"
                    Add-Content $OutFile "Line: $($i + 1)"
                    Add-Content $OutFile "Text: $($lines[$i].Trim())"
                    Add-Content $OutFile "----------------------------------------"
                }
            }
        }
    }
}

# Run searches
Search-And-Write -Terms $WeaponTerms   -OutFile $WeaponsOut
Search-And-Write -Terms $RoleTerms     -OutFile $RolesOut
Search-And-Write -Terms $InventoryTerms -OutFile $InventoryOut

Write-Host "Done! Output written to weapons.txt, roles.txt, inventory.txt" -ForegroundColor Green
