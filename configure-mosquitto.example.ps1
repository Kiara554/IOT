# ============================================================
# Smart CESI — Configuration Mosquitto (EXEMPLE)
# ============================================================
# 1. Copie ce fichier et renomme-le "configure-mosquitto.ps1"
# 2. Remplace les valeurs entre < > par tes vraies valeurs
# 3. Exécute-le dans un PowerShell Administrateur :
#    Clic droit sur PowerShell → "Exécuter en tant qu'administrateur"
#    puis : .\configure-mosquitto.ps1
# ============================================================

# Chemin vers le fichier de mots de passe Mosquitto
# Remplace <ton_utilisateur_windows> par ton nom de session Windows
$passwdFile = "C:\Users\<ton_utilisateur_windows>\.mosquitto\passwd"
$confFile   = "C:\Program Files\mosquitto\mosquitto.conf"

# Identifiants MQTT — doivent correspondre à MQTT_USER / MQTT_PASSWORD dans config.h
$mqttUser     = "fablab"
$mqttPassword = "SmartCESI2026"

# ------------------------------------------------------------
# 1. Créer le dossier et le fichier passwd
# ------------------------------------------------------------
New-Item -ItemType Directory -Force (Split-Path $passwdFile) | Out-Null
New-Item -ItemType File -Force $passwdFile | Out-Null
& "C:\Program Files\mosquitto\mosquitto_passwd.exe" -b $passwdFile $mqttUser $mqttPassword
Write-Host "[OK] Utilisateur $mqttUser créé dans $passwdFile"

# ------------------------------------------------------------
# 2. Écrire la config Mosquitto (sans BOM pour éviter les erreurs de parsing)
# ------------------------------------------------------------
$config = "listener 1883 0.0.0.0`nallow_anonymous false`npassword_file $passwdFile`nlog_dest stderr`nlog_type error`nlog_type warning`n"
[System.IO.File]::WriteAllText($confFile, $config, [System.Text.UTF8Encoding]::new($false))
Write-Host "[OK] Config écrite dans $confFile"

# ------------------------------------------------------------
# 3. Ouvrir le port 1883 dans le pare-feu Windows
# ------------------------------------------------------------
$ruleName = "Mosquitto MQTT 1883"
if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow | Out-Null
    Write-Host "[OK] Règle pare-feu ajoutée"
} else {
    Write-Host "[OK] Règle pare-feu déjà présente"
}

# ------------------------------------------------------------
# 4. Redémarrer Mosquitto et vérifier
# ------------------------------------------------------------
Restart-Service mosquitto
Start-Sleep -Seconds 2
$svc = Get-Service mosquitto
Write-Host "[OK] Service Mosquitto : $($svc.Status)"

Write-Host ""
Write-Host "Ports en écoute sur 1883 :"
netstat -an | Select-String ":1883"
Write-Host ""
Write-Host "Résultat attendu : TCP  0.0.0.0:1883  LISTENING"
