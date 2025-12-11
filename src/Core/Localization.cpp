#include "Localization.h"

#include <algorithm>
#include <unordered_set>

namespace
{

struct LanguageDefinition
{
        std::string code;
        std::string displayName;
        std::unordered_map<std::string, std::string> strings;
};

void AddTranslation(LanguageDefinition& language, const std::string& key, const std::string& value)
{
        language.strings.emplace(key, value);
}

LanguageDefinition BuildEnglishStrings()
{
        LanguageDefinition english{ "en", "English", {} };

        AddTranslation(english, "Toggle me with %s", "Toggle me with %s");
        AddTranslation(english, "Toggle Online with %s", "Toggle Online with %s");
        AddTranslation(english, "Toggle HUD with %s", "Toggle HUD with %s");
        AddTranslation(english, "Generate Debug Logs", "Generate Debug Logs");
        AddTranslation(english, "Write DEBUG.txt with detailed runtime information. Saved to settings.ini for future sessions.",
                "Write DEBUG.txt with detailed runtime information. Saved to settings.ini for future sessions.");
        AddTranslation(english, "Online", "Online");
        AddTranslation(english, "Gameplay settings", "Gameplay settings");
        AddTranslation(english, "Custom palettes", "Custom palettes");
        AddTranslation(english, "Hitbox overlay", "Hitbox overlay");
        AddTranslation(english, "FrameHistory", "FrameHistory");
        AddTranslation(english, "Framedata", "Framedata");
        AddTranslation(english, "Avatar settings", "Avatar settings");
        AddTranslation(english, "Controller Settings", "Controller Settings");
        AddTranslation(english, "Loaded settings.ini values", "Loaded settings.ini values");
        AddTranslation(english, "Current online players:", "Current online players:");
        AddTranslation(english, "<No data>", "<No data>");
        AddTranslation(english, "Avatar", "Avatar");
        AddTranslation(english, "Color", "Color");
        AddTranslation(english, "Accessory 1", "Accessory 1");
        AddTranslation(english, "Accessory 2", "Accessory 2");
        AddTranslation(english, "CONNECT TO NETWORK MODE FIRST", "CONNECT TO NETWORK MODE FIRST");
        AddTranslation(english, "FrameHistory help",
                "For each non-idle frame, display a column of rectangles with info about it. \r\n \r\nFor each player : \r\n = The first row displays player state. \r\n - Startup->green \r\n - Active->red \r\n - Recovery->blue \r\n - Blockstun->yellow \r\n - Hitstun->purple \r\n - Hard landing recovery->blush \r\n - Special: hard to classify states(e.g.dashes)->Aquamarine \r\n\r\n = Second row is for invul/armor. The position of the line segments indicates the attributes, and its color if invul or armor: \r\n - Invul->white \r\n - Armor->brown \r\n - H->top segment \r\n - B->middle segment \r\n - F->bottom segment \r\n - T->left segment \r\n - P->right segment \r\n");
        AddTranslation(english, "FrameHistory auto reset help",
                "block auto-reset on an idle frame: Do not overwrite automatically after an idle frame.");
        AddTranslation(english, "Box width", "Box width");
        AddTranslation(english, "Box height", "Box height");
        AddTranslation(english, "spacing", "spacing");
        AddTranslation(english, "Enable##framehistory_section", "Enable##framehistory_section");
        AddTranslation(english, "Auto Reset##Reset after each idle frame", "Auto Reset##Reset after each idle frame");
        AddTranslation(english, "Enable##framedata_section", "Enable##framedata_section");
        AddTranslation(english, "Advantage on stagger hit", "Advantage on stagger hit");
        AddTranslation(english, "YOU ARE NOT IN MATCH!", "YOU ARE NOT IN MATCH!");
        AddTranslation(english, "YOU ARE NOT IN TRAINING MODE OR REPLAY THEATER!", "YOU ARE NOT IN TRAINING MODE OR REPLAY THEATER!");
        AddTranslation(english, "YOU ARE NOT IN A MATCH, IN TRAINING MODE OR REPLAY THEATER!", "YOU ARE NOT IN A MATCH, IN TRAINING MODE OR REPLAY THEATER!");
        AddTranslation(english, "THERE WAS AN ERROR LOADING ONE/BOTH OF THE CHARACTERS", "THERE WAS AN ERROR LOADING ONE/BOTH OF THE CHARACTERS");
        AddTranslation(english, "Palette editor", "Palette editor");
        AddTranslation(english, "YOU ARE NOT IN MATCH! (Replay)", "YOU ARE NOT IN MATCH!");
        AddTranslation(english, "YOU ARE NOT IN REPLAY MENU!", "YOU ARE NOT IN REPLAY MENU!");
        AddTranslation(english, "YOU ARE NOT IN CHARACTER SELECTION!", "YOU ARE NOT IN CHARACTER SELECTION!");
        AddTranslation(english, "Hide HUD", "Hide HUD");
        AddTranslation(english, "Toggle Rewind", "Toggle Rewind");
        AddTranslation(english, "Replay Database", "Replay Database");
        AddTranslation(english, "Enable/Disable Upload", "Enable/Disable Upload");
        AddTranslation(english, "Discord", "Discord");
        AddTranslation(english, "Forum", "Forum");
        AddTranslation(english, "GitHub", "GitHub");
        AddTranslation(english, "Player1", "Player1");
        AddTranslation(english, "Player2", "Player2");
        AddTranslation(english, "Draw hitbox/hurtbox", "Draw hitbox/hurtbox");
        AddTranslation(english, "Draw origin", "Draw origin");
        AddTranslation(english, "Draw collision", "Draw collision");
        AddTranslation(english, "Draw throw/range boxes", "Draw throw/range boxes");
        AddTranslation(english, "The point in space where your character resides. \nImportant!: This is a single point, the render is composed of two lines to facilitate viewing, the actual point is where the two lines touch.",
                "The point in space where your character resides. \nImportant!: This is a single point, the render is composed of two lines to facilitate viewing, the actual point is where the two lines touch.");
        AddTranslation(english, "Defines collision between objects/characters. Also used for throw range checks.",
                "Defines collision between objects/characters. Also used for throw range checks.");
        AddTranslation(english, "Throw range help", "Throw Range Box(yellow): All throws require the throw range check. In order to pass this check the throw range box must overlap target's  collision box.\n\nMove Range Box(green): All throws and some moves require the move range check. In order to pass this check the move range box must overlap the target's origin point.\n\n\n\nHow do throws connect?\n\nIn order for a throw to connect you must have satisfy the following conditions:\n1: Both players must be on the ground or in the air. This is decided by their origin position, not sprite.\n2: You must pass the move range check.\n3: You must pass the throw range check.\n4: The hitbox of the throw must overlap the hurtbox of the target.\n5: The target must not be throw immune.\n");
        AddTranslation(english, "Freeze frame:", "Freeze frame:");
        AddTranslation(english, "Frame Count", "Frame Count");
        AddTranslation(english, "Reset", "Reset");
        AddTranslation(english, "Freeze animation", "Freeze animation");
        AddTranslation(english, "Stop timer", "Stop timer");
        AddTranslation(english, "Step frames", "Step frames");
        AddTranslation(english, "YOU ARE NOT IN TRAINING, VERSUS, OR REPLAY!", "YOU ARE NOT IN TRAINING, VERSUS, OR REPLAY!");
        AddTranslation(english, "Game Mode", "Game Mode");
        AddTranslation(english, "Enable##hitbox_overlay_section", "Enable##hitbox_overlay_section");
        AddTranslation(english, "Custom palette reload", "Reload all palettes");
        AddTranslation(english, "Enable##framehistory_section checkbox", "Enable##framehistory_section");
        AddTranslation(english, "Draw hitbox/hurtbox##hitbox_overlay_section", "Draw hitbox/hurtbox");
        AddTranslation(english, "Draw origin##hitbox_overlay_section", "Draw origin");
        AddTranslation(english, "Draw collision##hitbox_overlay_section", "Draw collision");
        AddTranslation(english, "Step frames##hitbox_overlay_section", "Step frames");
        AddTranslation(english, "Gameplay settings header", "Gameplay settings");
        AddTranslation(english, "Hide HUD checkbox", "Hide HUD");
        AddTranslation(english, "States", "States");
        AddTranslation(english, "Log", "Log");
        AddTranslation(english, "Replay Rewind", "Replay Rewind");
        AddTranslation(english, "Replay Upload", "Replay Upload");
        AddTranslation(english, "Enable##framehistory_section label", "Enable");
        AddTranslation(english, "Auto Reset label", "Auto Reset");
        AddTranslation(english, "FrameHistory width label", "Box width");
        AddTranslation(english, "FrameHistory height label", "Box height");
        AddTranslation(english, "FrameHistory spacing label", "spacing");
        AddTranslation(english, "FrameHistory not enabled message", "YOU ARE NOT IN A MATCH, IN TRAINING MODE OR REPLAY THEATER!");
        AddTranslation(english, "FrameHistory character load error", "THERE WAS AN ERROR LOADING ONE/BOTH OF THE CHARACTERS");
        AddTranslation(english, "Language", "Language");
        AddTranslation(english, "Language incomplete label", "%s - Incomplete (Missing %zu terms)");
        AddTranslation(english, "Language selection help", "Select a language for all UI text. Requires full dictionary coverage.");

        return english;
}

LanguageDefinition BuildSpanishStrings()
{
        LanguageDefinition spanish{ "es", "Español", {} };

        AddTranslation(spanish, "Toggle me with %s", "Activa la ventana con %s");
        AddTranslation(spanish, "Toggle Online with %s", "Abrir en línea con %s");
        AddTranslation(spanish, "Toggle HUD with %s", "Ocultar HUD con %s");
        AddTranslation(spanish, "Generate Debug Logs", "Generar registros de depuración");
        AddTranslation(spanish, "Write DEBUG.txt with detailed runtime information. Saved to settings.ini for future sessions.",
                "Escribe DEBUG.txt con información detallada de ejecución. Se guarda en settings.ini para futuras sesiones.");
        AddTranslation(spanish, "Online", "En línea");
        AddTranslation(spanish, "Gameplay settings", "Opciones de juego");
        AddTranslation(spanish, "Custom palettes", "Paletas personalizadas");
        AddTranslation(spanish, "Hitbox overlay", "Superposición de hitbox");
        AddTranslation(spanish, "FrameHistory", "Historial de cuadros");
        AddTranslation(spanish, "Framedata", "Datos de cuadro");
        AddTranslation(spanish, "Avatar settings", "Opciones de avatar");
        AddTranslation(spanish, "Controller Settings", "Opciones de control");
        AddTranslation(spanish, "Loaded settings.ini values", "Valores cargados de settings.ini");
        AddTranslation(spanish, "Current online players:", "Jugadores en línea actuales:");
        AddTranslation(spanish, "<No data>", "<Sin datos>");
        AddTranslation(spanish, "Avatar", "Avatar");
        AddTranslation(spanish, "Color", "Color");
        AddTranslation(spanish, "Accessory 1", "Accesorio 1");
        AddTranslation(spanish, "Accessory 2", "Accesorio 2");
        AddTranslation(spanish, "CONNECT TO NETWORK MODE FIRST", "CONECTA AL MODO EN LÍNEA PRIMERO");
        AddTranslation(spanish, "FrameHistory help",
                "Para cada cuadro no inactivo, muestra una columna de rectángulos con información. \r\n \r\nPara cada jugador: \r\n = La primera fila muestra el estado del jugador. \r\n - Inicio->verde \r\n - Activo->rojo \r\n - Recuperación->azul \r\n - Aturdimiento por bloqueo->amarillo \r\n - Aturdimiento por golpe->morado \r\n - Recuperación por caída dura->rosa \r\n - Especial: estados difíciles de clasificar (p.ej. dashes)->aguamarina \r\n\r\n = La segunda fila es para invulnerabilidad/armadura. La posición de los segmentos indica los atributos y su color si hay invul o armadura: \r\n - Invul->blanco \r\n - Armadura->marrón \r\n - H->segmento superior \r\n - B->segmento medio \r\n - F->segmento inferior \r\n - T->segmento izquierdo \r\n - P->segmento derecho \r\n");
        AddTranslation(spanish, "FrameHistory auto reset help",
                "Bloquear el reinicio automático en un cuadro inactivo: no sobrescribir automáticamente después de un cuadro inactivo.");
        AddTranslation(spanish, "Box width", "Ancho de caja");
        AddTranslation(spanish, "Box height", "Alto de caja");
        AddTranslation(spanish, "spacing", "Espaciado");
        AddTranslation(spanish, "Enable##framehistory_section", "Activar##framehistory_section");
        AddTranslation(spanish, "Auto Reset##Reset after each idle frame", "Reinicio automático##Reset after each idle frame");
        AddTranslation(spanish, "Enable##framedata_section", "Activar##framedata_section");
        AddTranslation(spanish, "Advantage on stagger hit", "Ventaja en golpe tambaleante");
        AddTranslation(spanish, "YOU ARE NOT IN MATCH!", "¡NO ESTÁS EN UNA PARTIDA!");
        AddTranslation(spanish, "YOU ARE NOT IN TRAINING MODE OR REPLAY THEATER!", "¡NO ESTÁS EN ENTRENAMIENTO NI EN REPLAY THEATER!");
        AddTranslation(spanish, "YOU ARE NOT IN A MATCH, IN TRAINING MODE OR REPLAY THEATER!", "¡NO ESTÁS EN PARTIDA, ENTRENAMIENTO NI REPLAY THEATER!");
        AddTranslation(spanish, "THERE WAS AN ERROR LOADING ONE/BOTH OF THE CHARACTERS", "HUBO UN ERROR AL CARGAR UNO O AMBOS PERSONAJES");
        AddTranslation(spanish, "Palette editor", "Editor de paletas");
        AddTranslation(spanish, "YOU ARE NOT IN MATCH! (Replay)", "¡NO ESTÁS EN UNA PARTIDA!");
        AddTranslation(spanish, "YOU ARE NOT IN REPLAY MENU!", "¡NO ESTÁS EN EL MENÚ DE REPLAY!");
        AddTranslation(spanish, "YOU ARE NOT IN CHARACTER SELECTION!", "¡NO ESTÁS EN LA SELECCIÓN DE PERSONAJE!");
        AddTranslation(spanish, "Hide HUD", "Ocultar HUD");
        AddTranslation(spanish, "Toggle Rewind", "Alternar rebobinado");
        AddTranslation(spanish, "Replay Database", "Base de datos de repeticiones");
        AddTranslation(spanish, "Enable/Disable Upload", "Activar/Desactivar subida");
        AddTranslation(spanish, "Discord", "Discord");
        AddTranslation(spanish, "Forum", "Foro");
        AddTranslation(spanish, "GitHub", "GitHub");
        AddTranslation(spanish, "Player1", "Jugador 1");
        AddTranslation(spanish, "Player2", "Jugador 2");
        AddTranslation(spanish, "Draw hitbox/hurtbox", "Dibujar hitbox/hurtbox");
        AddTranslation(spanish, "Draw origin", "Dibujar origen");
        AddTranslation(spanish, "Draw collision", "Dibujar colisión");
        AddTranslation(spanish, "Draw throw/range boxes", "Dibujar cajas de agarre/rango");
        AddTranslation(spanish, "The point in space where your character resides. \nImportant!: This is a single point, the render is composed of two lines to facilitate viewing, the actual point is where the two lines touch.",
                "El punto en el espacio donde reside tu personaje. \nImportante: es un único punto; el renderizado muestra dos líneas para facilitar la vista, el punto real está donde se cruzan.");
        AddTranslation(spanish, "Defines collision between objects/characters. Also used for throw range checks.",
                "Define la colisión entre objetos/personajes. También se usa para comprobar el alcance de agarres.");
        AddTranslation(spanish, "Throw range help", "Caja de rango de agarre (amarillo): todos los agarres requieren superar esta comprobación; la caja debe solaparse con la caja de colisión del objetivo.\n\nCaja de rango de movimiento (verde): todos los agarres y algunos movimientos requieren esta comprobación; la caja debe solaparse con el punto de origen del objetivo.\n\n\n\n¿Cómo conecta un agarre?\n\nPara que conecte debes cumplir: \n1: Ambos jugadores en tierra o aire (según su origen, no el sprite).\n2: Pasar la comprobación de rango de movimiento.\n3: Pasar la comprobación de rango de agarre.\n4: La hitbox del agarre debe solapar la hurtbox del objetivo.\n5: El objetivo no debe ser inmune a agarres.\n");
        AddTranslation(spanish, "Freeze frame:", "Congelar cuadro:");
        AddTranslation(spanish, "Frame Count", "Contador de cuadros");
        AddTranslation(spanish, "Reset", "Reiniciar");
        AddTranslation(spanish, "Freeze animation", "Congelar animación");
        AddTranslation(spanish, "Stop timer", "Detener temporizador");
        AddTranslation(spanish, "Step frames", "Avanzar cuadros");
        AddTranslation(spanish, "YOU ARE NOT IN TRAINING, VERSUS, OR REPLAY!", "¡NO ESTÁS EN ENTRENAMIENTO, VERSUS O REPLAY!");
        AddTranslation(spanish, "Game Mode", "Modo de juego");
        AddTranslation(spanish, "Enable##hitbox_overlay_section", "Activar##hitbox_overlay_section");
        AddTranslation(spanish, "Custom palette reload", "Recargar todas las paletas");
        AddTranslation(spanish, "Enable##framehistory_section checkbox", "Activar");
        AddTranslation(spanish, "Draw hitbox/hurtbox##hitbox_overlay_section", "Dibujar hitbox/hurtbox");
        AddTranslation(spanish, "Draw origin##hitbox_overlay_section", "Dibujar origen");
        AddTranslation(spanish, "Draw collision##hitbox_overlay_section", "Dibujar colisión");
        AddTranslation(spanish, "Step frames##hitbox_overlay_section", "Avanzar cuadros");
        AddTranslation(spanish, "Gameplay settings header", "Opciones de juego");
        AddTranslation(spanish, "Hide HUD checkbox", "Ocultar HUD");
        AddTranslation(spanish, "States", "Estados");
        AddTranslation(spanish, "Log", "Registro");
        AddTranslation(spanish, "Replay Rewind", "Rebobinado de repetición");
        AddTranslation(spanish, "Replay Upload", "Subida de repetición");
        AddTranslation(spanish, "Enable##framehistory_section label", "Activar");
        AddTranslation(spanish, "Auto Reset label", "Reinicio automático");
        AddTranslation(spanish, "FrameHistory width label", "Ancho de caja");
        AddTranslation(spanish, "FrameHistory height label", "Alto de caja");
        AddTranslation(spanish, "FrameHistory spacing label", "Espaciado");
        AddTranslation(spanish, "FrameHistory not enabled message", "¡NO ESTÁS EN UNA PARTIDA, ENTRENAMIENTO NI REPLAY THEATER!");
        AddTranslation(spanish, "FrameHistory character load error", "HUBO UN ERROR AL CARGAR UNO O AMBOS PERSONAJES");
        AddTranslation(spanish, "Language", "Idioma");
        AddTranslation(spanish, "Language incomplete label", "%s - Incompleto (faltan %zu términos)");
        AddTranslation(spanish, "Language selection help", "Selecciona un idioma para todo el texto de la interfaz. Requiere cobertura completa del diccionario.");

        return spanish;
}

std::vector<LanguageDefinition> BuildLanguageDefinitions()
{
        std::vector<LanguageDefinition> definitions;
        definitions.emplace_back(BuildEnglishStrings());
        definitions.emplace_back(BuildSpanishStrings());
        return definitions;
}

} // namespace

std::unordered_map<std::string, std::unordered_map<std::string, std::string>> Localization::m_languageStrings = {};
std::vector<LanguageOption> Localization::m_languageOptions = {};
std::string Localization::m_currentLanguage = "en";
std::string Localization::m_fallbackLanguage = "en";
bool Localization::m_initialized = false;

void Localization::Initialize(const std::string& requestedLanguage)
{
        if (!m_initialized)
        {
                LoadLanguageData();
                m_initialized = true;
        }

        RefreshLanguageStatuses();

        if (IsLanguageComplete(requestedLanguage))
        {
                m_currentLanguage = requestedLanguage;
        }
        else
        {
                m_currentLanguage = m_fallbackLanguage;
        }
}

const std::string& Localization::Translate(const std::string& key)
{
        const auto& languageMap = GetLanguageMap(m_currentLanguage);
        const auto& fallbackMap = GetLanguageMap(m_fallbackLanguage);

        auto it = languageMap.find(key);
        if (it != languageMap.end())
        {
                return it->second;
        }

        auto fallbackIt = fallbackMap.find(key);
        if (fallbackIt != fallbackMap.end())
        {
                return fallbackIt->second;
        }

        auto insertion = m_languageStrings[m_fallbackLanguage].emplace(key, key);
        return insertion.first->second;
}

const std::vector<LanguageOption>& Localization::GetAvailableLanguages()
{
        return m_languageOptions;
}

bool Localization::SetCurrentLanguage(const std::string& languageCode)
{
        if (!IsLanguageComplete(languageCode))
        {
                return false;
        }

        m_currentLanguage = languageCode;
        return true;
}

const std::string& Localization::GetCurrentLanguage()
{
        return m_currentLanguage;
}

bool Localization::IsLanguageComplete(const std::string& languageCode)
{
        return GetMissingKeyCount(languageCode) == 0;
}

size_t Localization::GetMissingKeyCount(const std::string& languageCode)
{
        const auto& fallbackMap = GetLanguageMap(m_fallbackLanguage);
        const auto& languageMap = GetLanguageMap(languageCode);

        size_t missingKeys = 0;
        for (const auto& required : fallbackMap)
        {
            if (languageMap.find(required.first) == languageMap.end())
            {
                    ++missingKeys;
            }
        }
        return missingKeys;
}

void Localization::LoadLanguageData()
{
        if (!m_languageStrings.empty())
        {
                return;
        }

        auto definitions = BuildLanguageDefinitions();
        for (const auto& language : definitions)
        {
                m_languageStrings.emplace(language.code, language.strings);
                LanguageOption option{};
                option.code = language.code;
                option.displayName = language.displayName;
                option.complete = false;
                option.missingKeys = 0;
                m_languageOptions.push_back(option);
        }
}

void Localization::RefreshLanguageStatuses()
{
        for (auto& option : m_languageOptions)
        {
                option.missingKeys = GetMissingKeyCount(option.code);
                option.complete = option.missingKeys == 0;
        }
}

const std::unordered_map<std::string, std::string>& Localization::GetLanguageMap(const std::string& languageCode)
{
        auto languageIt = m_languageStrings.find(languageCode);
        if (languageIt == m_languageStrings.end())
        {
                return m_languageStrings[m_fallbackLanguage];
        }
        return languageIt->second;
}

const std::string& L(const std::string& key)
{
        return Localization::Translate(key);
}

