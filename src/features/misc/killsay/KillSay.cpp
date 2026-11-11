#include "KillSay.hpp"

#include "config/Config.hpp"
#include "sdk/SDK.hpp"
#include "core/GlobalVars.hpp"
#include "features/misc/voicekill/VoiceKill.hpp"

#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cstddef>

namespace
{
static const char* const kKillPhrases[] = {
    "а вы че клины???",
    "обоссал мемюзера лол",
    "ты че там отлетел то",
    "lmao ur so ugly irl like bro doesnt it hurt to live like that, btw you know you can just end it all",
    "ROFL NICE *DEAD* HHHHHHHHHHHHHHHHHH",
    "take the cooldown and let your team surr retard",
    "go take some estrogen tranny",
    "uid police here present your user identification number right now",
    "нищий улетел",
    "*DEAD* пофикси нищ",
    "сразу видно юид иссуе хуле тут",
    "у мамки что кфг иссуе была шо тебя родила",
    "а ты и в жизни ньюкамыч????",
    "сука не позорься и ливни лол",
    "юид полиция подьехала открывай дверь уебыч",
    "набутылирован лол",
    "tranny holzed",
    "але ты там из хрущевки выеди а потом вырыгивай блять",
    "как там с мамкой комнату разделять АХАХАХХАХА как ты на акк накопил блять",
    "найс 0.5х0.5м комната блять ХАХАХАХА ТЫ ТАМ ЖЕ ДАЖЕ ПОВЕСИТЬСЯ НЕ МОЖЕШЬ МЕСТА НЕТ ПХПХПХППХ",
    "better buy the superior hack!",
    "на мыло и веревку то деньги есть нищ????",
    "whatcha shootin at retard",
    "опущены стяги, легион и.. А БЛЯТЬ ТЫЖ ТУТ ОПУЩ НАХУЙ ПХГАХААХАХАХАХА)))))))",
    "але какая с юидом ситуация)))",
    "бля че тут эта нищая собака заскулила",
    "не хотелось даже руки об тебя марать нищ сука",
    "ебать ты красиво на бутылку упал",
    "прости что без смазки)))",
    "алло это скорая? тут такая ситуация нищ упал))) ОЙ А ВЫ НИЩАМ ТО НЕ ПОМОГАЕТЕ?? ПОНЯТНО Я ПОЙДУ ТОГДА))))))))",
    "nice 0.5x0.5m room you poorfag, how the fuck did you afford an acc hhhhhh",
    "вырыгнись из окна нахуй боберхук юзер",
    "тяжело с мемсенсом наверно????",
    "imagine losing at video games couldn't ever be me",
    "але а противники то где???",
    "nice chromosome count you sell??",
    "nice thirdworldspeak ROFL",
    "как ты на пк накопил даже не знаю )))))))))",
    "iq больше двух будет пмнешь ок????",
    "НИХУЯ ТАМ НЬЮКАМЫЧА ОРОШИЛИ СТРУЕЙ МОЧИ АХАХХАХАХАХАХАХАХА",
    "дал юид за щеку проверяй",
    "nn4ik shat on",
    "хуя тебя опустили манька))))",
    "а у вас походу умирать это семейное)",
    "нахуя пидораса убил?",
    "чао персик дозревай",
    "уважаемый в тюрьме вы будете водолазом",
    "говори буду плохо говорить буду сосать, буду плохо сосать буду пересасывать",
    "долбаеб иди башмачки в сундучок школьный собирай",
    "ботинок ебаный чо слетел",
    "братик маме привет передай",
    "не противник",
    "а ты че клоун???",
    "я обоссал тебя (",
    "ты че там отлетел то?",
    "упал хуета ебаная , но в боди забрал да похуй все равно упал",
    "*DEAD* ливай с хвх (",
    "до связи башмак",
    "нищета глупейшая играть учись",
    "опущен сын твари",
    "нищий улетел",
    "*DEAD* пофикси нищ",
    "сразу видно кфг иссуе мб конфиг у меня прикупишь ?",
    "животное аддон скачай а то падаешь",
    "оттарабанен армянская королева",
    "сука не позорься и ливни",
    "улетел тапочек ебаный",
    "единицей свалился фуфлыжник",
    "зачем ты играешь тут безмозглый",
    "иди кумыса попей очередняра",
    "откисай сочняра",
    "АХАХА ЕБАТЬ У ТЕБЯ ЧЕРЕПНАЯ КОРОБКА ПРЯМ КАК [XML-RPC] No-Spread 24/7 | aim_ag_texture_2 ONLY!",
    "на мыло и веревку то деньги есть????",
    "ИЩИ СЕБЯ НА pornoeb.cc/sochniki",
    "свежий кабанчик",
    "до связи на подскоке кабанчик",
    "скажи маме сухарики купить долбаеб",
    "ебать ты красиво на бутылку присел , тебе дать альт ?",
    "прости что без смазки)",
    "алло это скорая? тут такая ситуация пареню который упал нужна скорая)",
    "ало ты мапу лузаешь дура очнись",
    "тяжело с кряком????",
    "ЕБУЧЕСТЬ ВТОРОГО РАЗРЯДА ВЫДВИЖЕНЕЦ ОТКИС",
    "але а противники то где???",
    "ты по легиту играешь ?",
    "ХУЕПРЫГАЛО ТУСОВОЧНОЕ КУДА ПОЛЕТЕЛО",
    "ты куда жертва козьего аборта",
    "iq?",
    "·٠●•۩۞۩ОтДыХаЙ (ٿ) НуБяРа۩۞۩•●٠·",
    "ты то куда лезешь сын фантомного стационарного спец изолированого металлформовочного механизма",
    "╭∩╮( ⚆ ʖ ⚆)╭∩╮ ДоПрыГался(ت)ДрУжоЧеК",
    "ez",
    "effortless",
    "1",
    "nice antiaim, you sell?",
    "you pay for that?",
    "refund right now",
    "consider suicide",
    "bro are u clean?",
    "another retard blasted",
    "hhhhhhhhhhhhhhhhhh 1, you pay for that? refund so maybe youll afford some food for your family thirdworld monkey",
    "paster abandoned the match and received a 7 day competitive matchmaking cooldown",
    "freeqn.net/refund.php",
    "refund your rainbowhook right now pasteuser dog",
    "бля эт пиздец че какие то там нищи с мемсенсом рыгают блять",
    "тебе права голоса не давали thirdworlder ебаный",
    "на завод иди",
    "JAJAJAJJAJA NICE RAINBOWPASTE ROFL",
    "140er????",
    "get good get vantap4ik",
    "1 but all you need to fix your problems is a rope and a chair you ugly shit",
    "who (kto) are you (nn4ik) wattafak mens???????",
    "must be an uid issue",
    "holy shit consider refunding your trash paste rofl",
    "hello please refund your subpar product",
    "ебать тебя унесло",
    "рефандни пожалуйста",
    "на бутылку русак",
    "a вы (you) сэр собственно кто (who)?",
    "бля пиздос может рефнешь???",
    "как там жизнь с рупастой??????",
    "але может не будешь тратить мамкину зарплату на говнопасты???",
    "stop spending your lunch money on shitpastes retard",
    "бля я рядом только прошел а ты уже упал АУУ НИЩ С ТОБОЙ ВСЕ ХОРОШО??????????))))))",
    "ой нищий упал щас скорую вызовем",
    "ой а кто (who) ты (you) такой вотзефак мен))))))",
    "thats going in my media compilation right there get shamed retard rofl",
    "imagine the only thing you eat being bullets man being a thirdworlder must suck rofl",
    "so fucking ez",
    "bot_kick",
    "where the enemies at????",
    "але найс упал нищ ЛОООООООЛ",
    "с тобой там все хорошо????????????? а да ты нищ нахуя я спрашиваю ПХАХАХАХАХХА",
    "жаль конечно что против нищей играть надо)))",
    "тебе даже спин не поможет блять, нахуй ты вообще живешь",
    "ты можешь заселлить лишнюю хромосому???",
    "научи потом как так сосать на хвх",
    "когда не накопил на гормоны и чулки)))))))))))))",
    "как там жизнь на мамкину пенсию???????",
    "але я бот_кик в консоль вроде прописал а вас там не кикнуло эт че баг??)))))))))",
    "крякоюзер down, на завод нахуй",
    "я не понял ты такой жирный потомучто дошики каждый день жрешь???? нормальную работу найди может на еду денег хватит))))))))))))",
    "насрал тебе в ротешник проверяй",
    "нихуя ты там как самолет отлетел ХАХАХХАХАХАХАХХХААХАА",
    "БЛЯ НИЩ ХУЯК ХУЯК И ТЕБЯ НЕТ КАК МОЖНО ТАКИМ БЫТЬ?????? ОБЬЯСНИСЬ БЛЯТЬ",
    "держи зонтик ☂, тебя обоссали",
    "Держи ✈ и лети нахуй !",
    "слишком сочный для Djamic.technologies",
    "сьебался нахуй таракан усатый",
    "мать твою ебал",
    "нахуй ты упал иди вставай и на завод",
    "не по сезону шуршишь фраер",
    "ИЗИ ЧМО ЕБАНОЕ",
    "ливай блять не позорься",
    "AХАХ ПИДОР УПАЛ С ВАНВЕЯ ХАХАХА ОНЛИ БАИМ В БОДИ ПОТЕЕТ НА ФД АХА",
    "АХАХА УЛЕТЕЛ ЧМОШНИК",
    "1 МАТЬ ЖАЛЬ",
    "тебе права голоса не давали thirdworlder ебаный",
    "на завод иди",
    "не не он опять упал на конлени",
    "вставай заебал , завтра в школу",
    "гет гуд гет иди нахуй",
    "by SANCHEZj hvh boss",
    "ну нет почему он ложится когда я прохожу",
    "у тебя ник нейм адео?",
    "парень тебе ник придумать?",
    "такой тупой :(",
    "хватит получать хс ,лучше возьми свою руку и иди дрочи",
    "нет нет этот крякер такой смешной",
    "1 сын шлюхи",
    "1 мать твою ебал",
    "преобрети мой кфг клоун",
    "об кафель голову разбил?",
    "мать твою ебал",
    "хуесос дальше адайся ко мне",
    "ещё раз позови к себе на бекап",
    "не противник",
    "ник нейм дориан(",
    "iq ?",
    "упал = минус мать",
    "не пиши мне",
    "жаль конечно что против тебя играю, но куда деваться",
    "адиничкой упал",
    "сынок зачем тебе это ?",
    "давно в рот берёшь?",
    "мне можно",
    "ты меня так заебал , ливни уже",
    "ничему жизнь не учит (",
    "я не понял, ты такой жирный потомучто дошики каждый день жрешь?",
    "братка го я тебе бекап позову",
    "толстяк даже пройти спокойно не может",
};
static const size_t kKillCount = sizeof( kKillPhrases ) / sizeof( kKillPhrases[ 0 ] );

static const char* const kDeathGroups[][ 3 ] = {
    {"ну он же реально","подрывает себя","и я не могу попасть"},
    {"ты че ваще ебанутый?", "я твою мать ебал", "ебаный тупой ублюдок"},
    {"в этом раунде большой огурец не прокерил", "значит прокерит в следующем"},
    {"опять говно убивает", "я просто не могу уже"},
    {"ну как ты мои антиаимы тапаешь в огурце??", "че ебу дал?"},
    {"я не могу в тебя попасть", "сделаешь мне такие же пресетики?", "на big pickle"},
    {"НУ ЧТО ТЫ ЗА ДОЛБОЕБ", "БЛЯДИНА ЕБАНА Я ТЬЯВОЮ МАТЬ ЕБАЛ"},
    {"сын шлюхи ебаной я просто не знаю как тебя ещё назвать", "за твои действия пидарас"},
    {"какой же ты ХУЕСОС", "НУ Я МИССАЮ ЕМУ В РУКУ А ОН ДУМАЕТ", "ЧТО ЭТО ЛЦ"},
    {"шлюха да что ты себе позволяешь"},
    {"ну всё пидрила", "!аdmin"},
    {"ты ахуел пидар", "далбаёб", nullptr},
    {",kznm", "как включять аим", nullptr},
    {"сервак тепнул", nullptr, nullptr},
    {"ты какую луа на кастом резик юзаешь?", nullptr, nullptr},
    {"CERF", "я щас с огорода приду", "зайду пизды те дам"},
    {"тебе повезло шлюха", "ты меня убил только пушто у меня нос зачесался", nullptr},
    {"нихуя се у тя крутилка", "подскажи настры", nullptr},
    {"как ты меня аншотнуло чмо", "яж дт прожал", nullptr},
    {"ты как меня убил хуйпасос", "Rayzen#5311 аддай шлюха", nullptr},
    {"ты как меня убил", "!аdmin  ", "ливай шлюха 5 сек даю"},
    {"блядь ебалн", "сука тимейт куда то убежал", nullptr},
    {"ну нет мусор", "блядь что ты делаешь бот ссаный", nullptr},
    {"о неееееет", "сука хуйпачос ебаный ты заебал 1в пикать", nullptr},
    {"не ливай хуесос", "щас поиграем долбаеб нищий", nullptr},
    {"убогий школьник", "расскажи сколько ванвеев сегодня тапнул", "хуйпоклык обрыганский"},
    {"блядт мусор господи", "не ливай нахуй щас ты землю толкать пойдёшь", "опарыш бля"},
    {"о боже пидрил", "снова лишний вес нашей планеты убил", nullptr},
    {"ты меня убил хуесос,но за аллаха респект", nullptr, nullptr},
    {"не ливай мусор я щас тебя выебу", "можешь идти аллаху молиться на некст раунд", nullptr},
    {"инфузория ебаная ты что снова наделал", "ты щас такой пизды получишь отвечаю", "компьютер неделю включать не будешь"},
    {"глупый даун", "после некст раунда ты захочешь с хвх ливнуть", nullptr},
    {"убил меня?", "теперь радуйся сиди две минуты", "хуесос нищий"},
    {"АХАХАХАХ", "ТОЛЬКО НЕ ЭТО", "тупорылый еблан"},
    {"миндмг", "слетел", nullptr},
    {"отмена", "там же прыгает", "настоящий сын шлюхи"},
    {"чит же видит какой он бездарный", "и даёт шанс этому хуесосу", nullptr},
    {"не ожидал что ты настолько тупорылый", "запишу тебя в тетрадь сочников", nullptr},
    {"ебанный выблядок", "как же тебе везёт", nullptr},
    {"чмырь ебанный", "рн 5х5 на 5к евро", nullptr},
};
static const size_t kDeathGroupCount = sizeof( kDeathGroups ) / sizeof( kDeathGroups[ 0 ] );

    size_t g_killIndex       = 0;
    size_t g_deathGroupIndex = 0;


    // ============================================================
    //  IGameEventListener — the engine (vtable slot 1) calls
    //  FireGameEvent for every event we registered for.
    // ============================================================
    class KillListener final : public SDK::IGameEventListener
    {
    public:
        KillListener() = default;
        ~KillListener() override = default;

        void FireGameEvent( SDK::IGameEvent* event ) override
        {
            if ( !event )
                return;

            if ( std::strcmp( event->GetName(), "player_death" ) != 0 )
                return;

            const int attacker     = event->GetInt( "attacker" );
            const int victim       = event->GetInt( "userid" );
            const int localUserId  = GetLocalUserId();
            if ( localUserId <= 0 )
                return;

            if ( !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
                return;

            const UINT64 now = GetTickCount64();

            // Voice Kill is independent from chat KillSay. Local attacker
            // selects the kill library; local victim selects voice_deaths.
            // Suicide/world damage does not cross-trigger either library.
            if ( attacker == localUserId && attacker > 0 && victim != attacker )
                VoiceKill::OnLocalKill();
            else if ( victim == localUserId && attacker > 0 && attacker != victim )
                VoiceKill::OnLocalDeath();

            const auto& ks = g_config.settings.misc.killsay;
            if ( !ks.enabled )
                return;

// ---- We died: send the current victim group as ONE chat message. ----
            if ( victim == localUserId )
            {
                if ( now - s_lastDeathTime < static_cast<UINT64>( ks.cooldown_ms ) )
                    return;
                s_lastDeathTime = now;

                const size_t idx = g_deathGroupIndex % kDeathGroupCount;
                g_deathGroupIndex = ( g_deathGroupIndex + 1 ) % kDeathGroupCount;

                char msg[ 512 ];
                msg[ 0 ] = '\0';
                for ( size_t i = 0; i < 3; ++i )
                {
                    const char* phrase = kDeathGroups[ idx ][ i ];
                    if ( !phrase || !phrase[ 0 ] )
                        continue;

                    if ( msg[ 0 ] )
                        _snprintf_s( msg, sizeof( msg ), _TRUNCATE, "%s %s", msg, phrase );
                    else
                        _snprintf_s( msg, sizeof( msg ), _TRUNCATE, "%s", phrase );
                }

                if ( msg[ 0 ] )
                {
                    char cmd[ 544 ];
                    _snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "say %s", msg );
                    SDK::g_pEngine->ClientCmd( cmd );
                }
                return;
            }

            // ---- We got a kill: one phrase, cycling in order. ----
            if ( attacker != localUserId || attacker <= 0 || victim == attacker )
                return;

            if ( now - s_lastKillTime < static_cast<UINT64>( ks.cooldown_ms ) )
                return;
            s_lastKillTime = now;

            const char* phrase = nullptr;
            if ( kKillCount > 0 )
            {
                phrase = kKillPhrases[ g_killIndex % kKillCount ];
                g_killIndex = ( g_killIndex + 1 ) % kKillCount;
            }

            char cmd[ 224 ];
            if ( phrase && phrase[ 0 ] )
                _snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "say %s", phrase );
            else
                _snprintf_s( cmd, sizeof( cmd ), _TRUNCATE, "say %s", ks.message );

            SDK::g_pEngine->ClientCmd( cmd );
        }

    private:
        // Local player's userid. The engine's GetPlayerInfo copies 132
        // bytes of player_info_t; the userid sits at offset +32
        // (name[32] then a 4-byte int).
        static int GetLocalUserId()
        {
            if ( !SDK::g_pEngine )
                return 0;

            const int localIdx = SDK::g_pEngine->GetLocalPlayer();
            if ( localIdx <= 0 )
                return 0;

            SDK::player_info_t info{};
            if ( !SDK::g_pEngine->GetPlayerInfo( localIdx, &info ) )
                return 0;

            // Chromium/CSGO-style layout: int userid at +32 after 32-char name.
            return *reinterpret_cast<const int*>( reinterpret_cast<const char*>( &info ) + 32 );
        }

        static UINT64 s_lastKillTime;
        static UINT64 s_lastDeathTime;
    };

UINT64 KillListener::s_lastKillTime  = 0;
    UINT64 KillListener::s_lastDeathTime = 0;

    // The engine stores bServerSide^1 in the listener record:
    //   bServerSide=true  -> 0 : receives FireEvent (server-side events)
    //   bServerSide=false -> 1 : receives FireEventClientSide (client events)
    // Different records -> need one listener object per side.
    KillListener g_listenerServer; // bServerSide = true
    KillListener g_listenerClient; // bServerSide = false
    bool         g_listening = false;
} // namespace

namespace KillSay
{
    bool Init()
    {
        if ( !SDK::g_pGameEventManager )
            return false;

        // Both registration paths are kept — the game fires kill events
        // through FireEventClientSide on this engine.
        const bool rs = SDK::g_pGameEventManager->AddListener( &g_listenerServer, "player_death", true );
        const bool rc = SDK::g_pGameEventManager->AddListener( &g_listenerClient, "player_death", false );

        g_listening = rs || rc;
        return g_listening;
    }

void Shutdown()
    {
        if ( g_listening && SDK::g_pGameEventManager )
        {
            SDK::g_pGameEventManager->RemoveListener( &g_listenerServer );
            SDK::g_pGameEventManager->RemoveListener( &g_listenerClient );
        }

        g_listening = false;
    }
} // namespace KillSay