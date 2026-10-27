#include "strings.hpp"

#include <windows.h>

#include "win_util.hpp"

namespace aurora::setup {
namespace {

constexpr LanguageInfo kLanguages[] = {
    {Language::En, L"English (United States)", L"en-US", 0x09},
    {Language::Ro, L"Română (România)", L"ro-RO", 0x18},
    {Language::De, L"Deutsch (Deutschland)", L"de-DE", 0x07},
    {Language::Fr, L"Français (France)", L"fr-FR", 0x0C},
    {Language::Es, L"Español (España)", L"es-ES", 0x0A},
};

constexpr Strings kEnglish{
    L"Aurora Browser Setup",
    L"Fast. Private. Yours.",
    L"Select Language",
    L"Choose the language for the installation wizard and Aurora Browser.",
    L"Welcome to the\nAurora Browser Setup",
    L"This wizard will guide you through the installation of Aurora Browser.",
    L"Click Next to continue or Cancel to exit.",
    L"Choose Install Location",
    L"Choose the folder in which to install Aurora Browser.",
    L"Browse...",
    L"Space required: %s",
    L"Space available: %s",
    L"Choose the Aurora install folder",
    L"Choose Components",
    L"Select the components you want to install.",
    L"Aurora Browser (required)",
    L"Desktop shortcut",
    L"Start Menu shortcut",
    L"The main browser application with all features.",
    L"Installing",
    L"Please wait while Aurora Browser is being installed.",
    L"Preparing...",
    L"Installing files...",
    L"Installation Complete",
    L"Aurora Browser has been successfully installed on your computer.",
    L"Aurora Browser has been successfully updated to version %s.",
    L"Launch Aurora Browser",
    L"Create desktop shortcut",
    L"Installation Failed",
    L"An unexpected error occurred while installing Aurora Browser.",
    L"Error code: %s",
    L"Details were written to %s",
    L"Thank you for choosing Aurora!",
    L"Browse the web, your way.",
    L"Back",
    L"Next",
    L"Cancel",
    L"Finish",
    L"Close",
    L"Launch Aurora",
    L"Cancel installation?",
    L"Files already copied to the temporary folder will be removed. Your existing Aurora installation will not be changed.",
    L"Aurora Browser is running",
    L"Close all Aurora Browser windows, then click Retry.",
    L"Administrator permission required",
    L"The selected folder needs administrator permission. Restart the installer with elevated rights?",
    L"Invalid install folder",
    L"Choose a full path to a folder. Drive roots and Windows system folders cannot be used.",
    L"Shortcut could not be created",
    L"Aurora could not be started",
    L"Updating Aurora",
    L"Please wait while Aurora Browser is being updated.",
    L"Installation canceled.",
};

constexpr Strings kRomanian{
    L"Instalare Aurora Browser",
    L"Rapid. Privat. Al tău.",
    L"Selectați limba",
    L"Alegeți limba pentru programul de instalare și pentru Aurora Browser.",
    L"Bine ați venit la\ninstalarea Aurora Browser",
    L"Acest program vă ghidează prin instalarea Aurora Browser.",
    L"Faceți clic pe Următorul pentru a continua sau pe Anulare pentru a ieși.",
    L"Alegeți locația de instalare",
    L"Alegeți folderul în care se instalează Aurora Browser.",
    L"Răsfoire...",
    L"Spațiu necesar: %s",
    L"Spațiu disponibil: %s",
    L"Alegeți folderul de instalare Aurora",
    L"Alegeți componentele",
    L"Selectați componentele pe care doriți să le instalați.",
    L"Aurora Browser (obligatoriu)",
    L"Scurtătură pe desktop",
    L"Scurtătură în meniul Start",
    L"Aplicația principală de navigare, cu toate funcțiile.",
    L"Se instalează",
    L"Vă rugăm să așteptați instalarea Aurora Browser.",
    L"Se pregătește...",
    L"Se instalează fișierele...",
    L"Instalare finalizată",
    L"Aurora Browser a fost instalat cu succes pe computer.",
    L"Aurora Browser a fost actualizat cu succes la versiunea %s.",
    L"Lansează Aurora Browser",
    L"Creează o scurtătură pe desktop",
    L"Instalarea a eșuat",
    L"A apărut o eroare neașteptată în timpul instalării Aurora Browser.",
    L"Cod de eroare: %s",
    L"Detaliile au fost scrise în %s",
    L"Vă mulțumim că ați ales Aurora!",
    L"Navigați web-ul în felul dumneavoastră.",
    L"Înapoi",
    L"Următorul",
    L"Anulare",
    L"Finalizare",
    L"Închidere",
    L"Lansează Aurora",
    L"Anulați instalarea?",
    L"Fișierele copiate deja în folderul temporar vor fi șterse. Instalarea existentă a Aurora nu va fi modificată.",
    L"Aurora Browser rulează",
    L"Închideți toate ferestrele Aurora Browser, apoi faceți clic pe Reîncercare.",
    L"Este necesară permisiunea de administrator",
    L"Folderul selectat necesită permisiune de administrator. Reporniți programul de instalare cu drepturi de administrator?",
    L"Folder de instalare nevalid",
    L"Alegeți o cale completă către un folder. Rădăcinile de disc și folderele de sistem Windows nu pot fi folosite.",
    L"Scurtătura nu a putut fi creată",
    L"Aurora nu a putut fi pornit",
    L"Se actualizează Aurora",
    L"Vă rugăm să așteptați actualizarea Aurora Browser.",
    L"Instalare anulată.",
};

constexpr Strings kGerman{
    L"Aurora Browser Setup",
    L"Schnell. Privat. Deins.",
    L"Sprache auswählen",
    L"Wählen Sie die Sprache für das Installationsprogramm und Aurora Browser.",
    L"Willkommen bei der\nInstallation von Aurora Browser",
    L"Dieser Assistent führt Sie durch die Installation von Aurora Browser.",
    L"Klicken Sie auf Weiter, um fortzufahren, oder auf Abbrechen, um den Vorgang zu beenden.",
    L"Installationsordner wählen",
    L"Wählen Sie den Ordner, in dem Aurora Browser installiert werden soll.",
    L"Durchsuchen...",
    L"Benötigter Speicherplatz: %s",
    L"Verfügbarer Speicherplatz: %s",
    L"Aurora-Installationsordner wählen",
    L"Komponenten auswählen",
    L"Wählen Sie die zu installierenden Komponenten aus.",
    L"Aurora Browser (erforderlich)",
    L"Desktop-Verknüpfung",
    L"Startmenü-Verknüpfung",
    L"Die Hauptbrowser-Anwendung mit allen Funktionen.",
    L"Wird installiert",
    L"Bitte warten Sie, während Aurora Browser installiert wird.",
    L"Wird vorbereitet...",
    L"Dateien werden installiert...",
    L"Installation abgeschlossen",
    L"Aurora Browser wurde erfolgreich auf diesem Computer installiert.",
    L"Aurora Browser wurde erfolgreich auf Version %s aktualisiert.",
    L"Aurora Browser starten",
    L"Desktop-Verknüpfung erstellen",
    L"Installation fehlgeschlagen",
    L"Bei der Installation von Aurora Browser ist ein unerwarteter Fehler aufgetreten.",
    L"Fehlercode: %s",
    L"Details wurden in %s geschrieben",
    L"Danke, dass Sie Aurora gewählt haben!",
    L"Surfen Sie im Web, wie Sie es möchten.",
    L"Zurück",
    L"Weiter",
    L"Abbrechen",
    L"Fertig stellen",
    L"Schließen",
    L"Aurora starten",
    L"Installation abbrechen?",
    L"Bereits in den temporären Ordner kopierte Dateien werden entfernt. Ihre vorhandene Aurora-Installation bleibt unverändert.",
    L"Aurora Browser läuft",
    L"Schließen Sie alle Aurora Browser-Fenster und klicken Sie dann auf Wiederholen.",
    L"Administratorberechtigung erforderlich",
    L"Der ausgewählte Ordner erfordert Administratorberechtigungen. Installationsprogramm mit erhöhten Rechten neu starten?",
    L"Ungültiger Installationsordner",
    L"Wählen Sie einen vollständigen Pfad zu einem Ordner. Laufwerksstammverzeichnisse und Windows-Systemordner können nicht verwendet werden.",
    L"Verknüpfung konnte nicht erstellt werden",
    L"Aurora konnte nicht gestartet werden",
    L"Aurora wird aktualisiert",
    L"Bitte warten Sie, während Aurora Browser aktualisiert wird.",
    L"Installation abgebrochen.",
};

constexpr Strings kFrench{
    L"Configuration d'Aurora Browser",
    L"Rapide. Privé. À vous.",
    L"Choisir la langue",
    L"Choisissez la langue de l'assistant d'installation et d'Aurora Browser.",
    L"Bienvenue dans\nl'installation d'Aurora Browser",
    L"Cet assistant va vous guider lors de l'installation d'Aurora Browser.",
    L"Cliquez sur Suivant pour continuer ou sur Annuler pour quitter.",
    L"Choisir le dossier d'installation",
    L"Choisissez le dossier dans lequel installer Aurora Browser.",
    L"Parcourir...",
    L"Espace requis : %s",
    L"Espace disponible : %s",
    L"Choisir le dossier d'installation d'Aurora",
    L"Choisir les composants",
    L"Sélectionnez les composants à installer.",
    L"Aurora Browser (obligatoire)",
    L"Raccourci sur le Bureau",
    L"Raccourci dans le menu Démarrer",
    L"L'application principale de navigation avec toutes ses fonctions.",
    L"Installation",
    L"Veuillez patienter pendant l'installation d'Aurora Browser.",
    L"Préparation...",
    L"Installation des fichiers...",
    L"Installation terminée",
    L"Aurora Browser a été installé avec succès sur cet ordinateur.",
    L"Aurora Browser a été mis à jour avec succès vers la version %s.",
    L"Lancer Aurora Browser",
    L"Créer un raccourci sur le Bureau",
    L"Échec de l'installation",
    L"Une erreur inattendue s'est produite pendant l'installation d'Aurora Browser.",
    L"Code d'erreur : %s",
    L"Les détails ont été écrits dans %s",
    L"Merci d'avoir choisi Aurora !",
    L"Naviguez sur le Web à votre façon.",
    L"Retour",
    L"Suivant",
    L"Annuler",
    L"Terminer",
    L"Fermer",
    L"Lancer Aurora",
    L"Annuler l'installation ?",
    L"Les fichiers déjà copiés dans le dossier temporaire seront supprimés. Votre installation existante d'Aurora ne sera pas modifiée.",
    L"Aurora Browser est en cours d'exécution",
    L"Fermez toutes les fenêtres d'Aurora Browser, puis cliquez sur Réessayer.",
    L"Autorisation administrateur requise",
    L"Le dossier sélectionné nécessite une autorisation administrateur. Redémarrer l'installateur avec des droits élevés ?",
    L"Dossier d'installation non valide",
    L"Choisissez un chemin complet vers un dossier. Les racines de lecteur et les dossiers système Windows ne sont pas autorisés.",
    L"Le raccourci n'a pas pu être créé",
    L"Aurora n'a pas pu être démarré",
    L"Mise à jour d'Aurora",
    L"Veuillez patienter pendant la mise à jour d'Aurora Browser.",
    L"Installation annulée.",
};

constexpr Strings kSpanish{
    L"Configuración de Aurora Browser",
    L"Rápido. Privado. Tuyo.",
    L"Seleccionar idioma",
    L"Elige el idioma del asistente de instalación y de Aurora Browser.",
    L"Te damos la bienvenida a la\ninstalación de Aurora Browser",
    L"Este asistente te guiará durante la instalación de Aurora Browser.",
    L"Haz clic en Siguiente para continuar o en Cancelar para salir.",
    L"Elegir ubicación de instalación",
    L"Elige la carpeta en la que se instalará Aurora Browser.",
    L"Examinar...",
    L"Espacio necesario: %s",
    L"Espacio disponible: %s",
    L"Elegir la carpeta de instalación de Aurora",
    L"Elegir componentes",
    L"Selecciona los componentes que quieres instalar.",
    L"Aurora Browser (obligatorio)",
    L"Acceso directo en el escritorio",
    L"Acceso directo en el menú Inicio",
    L"La aplicación principal de navegación con todas las funciones.",
    L"Instalando",
    L"Espera mientras se instala Aurora Browser.",
    L"Preparando...",
    L"Instalando archivos...",
    L"Instalación completada",
    L"Aurora Browser se ha instalado correctamente en el equipo.",
    L"Aurora Browser se ha actualizado correctamente a la versión %s.",
    L"Iniciar Aurora Browser",
    L"Crear acceso directo en el escritorio",
    L"Error de instalación",
    L"Se ha producido un error inesperado durante la instalación de Aurora Browser.",
    L"Código de error: %s",
    L"Los detalles se han guardado en %s",
    L"¡Gracias por elegir Aurora!",
    L"Navega por la web a tu manera.",
    L"Atrás",
    L"Siguiente",
    L"Cancelar",
    L"Finalizar",
    L"Cerrar",
    L"Iniciar Aurora",
    L"¿Cancelar la instalación?",
    L"Se eliminarán los archivos ya copiados a la carpeta temporal. Tu instalación existente de Aurora no cambiará.",
    L"Aurora Browser se está ejecutando",
    L"Cierra todas las ventanas de Aurora Browser y haz clic en Reintentar.",
    L"Se requiere permiso de administrador",
    L"La carpeta seleccionada requiere permisos de administrador. ¿Reiniciar el instalador con permisos elevados?",
    L"Carpeta de instalación no válida",
    L"Elige una ruta completa a una carpeta. No se pueden usar las raíces de unidad ni las carpetas del sistema de Windows.",
    L"No se pudo crear el acceso directo",
    L"No se pudo iniciar Aurora",
    L"Actualizando Aurora",
    L"Espera mientras se actualiza Aurora Browser.",
    L"Instalación cancelada.",
};

} // namespace

const LanguageInfo* language_list() { return kLanguages; }

int language_count() {
    return static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
}

const Strings& strings(Language language) {
    switch (language) {
        case Language::Ro: return kRomanian;
        case Language::De: return kGerman;
        case Language::Fr: return kFrench;
        case Language::Es: return kSpanish;
        case Language::En:
        default: return kEnglish;
    }
}

int language_index(Language language) {
    for (int i = 0; i < language_count(); ++i) {
        if (kLanguages[i].id == language) return i;
    }
    return 0;
}

Language language_from_tag(const std::wstring& tag) {
    const std::wstring lower = lower_ascii(tag);
    for (int i = 0; i < language_count(); ++i) {
        const std::wstring candidate = lower_ascii(kLanguages[i].tag);
        if (lower == candidate) return kLanguages[i].id;
        if (lower.size() >= 2 &&
            candidate.compare(0, 2, lower.substr(0, 2)) == 0) {
            return kLanguages[i].id;
        }
    }
    return Language::En;
}

Language system_language() {
    const unsigned short primary =
        PRIMARYLANGID(GetUserDefaultUILanguage());
    for (int i = 0; i < language_count(); ++i) {
        if (kLanguages[i].primary_language == primary) return kLanguages[i].id;
    }
    return Language::En;
}

std::wstring format_string(const wchar_t* pattern, const std::wstring& value) {
    std::wstring out(pattern ? pattern : L"");
    const size_t pos = out.find(L"%s");
    if (pos != std::wstring::npos) out.replace(pos, 2, value);
    return out;
}

} // namespace aurora::setup
