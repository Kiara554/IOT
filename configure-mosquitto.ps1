# Lancer ce script en tant qu'Administrateur :
# Clic droit sur PowerShell → "Exécuter en tant qu'administrateur"
# puis : cd "C:\Users\33609\Documents\CESI\Année 4\Blocs\IOT\projet" ; .\configure-mosquitto.ps1

$passwdFile = "C:\Users\33609\.mosquitto\passwd"
$confFile   = "C:\Program Files\mosquitto\mosquitto.conf"

# 1. Créer le fichier passwd s'il n'existe pas
if (-not (Test-Path $passwdFile)) {
    New-Item -ItemType Directory -Force (Split-Path $passwdFile) | Out-Null
    New-Item -ItemType File -Force $passwdFile | Out-Null
    & "C:\Program Files\mosquitto\mosquitto_passwd.exe" -b $passwdFile fablab SmartCESI2026
    Write-Host "[OK] Utilisateur fablab créé dans $passwdFile"
} else {
    Write-Host "[OK] Fichier passwd existant : $passwdFile"
}

# 2. Écrire la config Mosquitto
$config = @"
# Smart CESI — FabLab Monitor
listener 1883 0.0.0.0
allow_anonymous false
password_file $passwdFile
log_dest stderr
log_type error
log_type warning
log_type notice
"@

Set-Content -Path $confFile -Value $config -Encoding UTF8
Write-Host "[OK] Config écrite dans $confFile"

# 3. Redémarrer le service
Restart-Service mosquitto
Start-Sleep -Seconds 2
$svc = Get-Service mosquitto
Write-Host "[OK] Service Mosquitto : $($svc.Status)"

# 4. Vérification du port
$port = netstat -an | Select-String ":1883"
Write-Host "`nPorts en écoute :"
$port | ForEach-Object { Write-Host "  $_" }
