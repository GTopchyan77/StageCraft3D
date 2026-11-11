# MIDI և OSC. վահանակի կառավարում կոնտրոլերից կամ պլանշետից
keywords: հեռակառավարում, կոնտրոլեր, պլանշետ, ֆեյդեր, ուսուցում, հետադարձ կապ, midi, osc, remote, controller, fader box, touchosc, nanokontrol, learn, feedback
panel: StageCraft.Panel.Preferences

MIDI ֆեյդերային սարքը կամ պլանշետի OSC հավելվածը կարող է կառավարել վահանակը՝ cue GO և BACK, տասը playback-ները (մակարդակ, flash, GO, release), Grand Master-ը, Blackout-ը և Audio master-ը։ Յուրաքանչյուր շարժում անցնում է նույն կանոններով, ինչ վահանակի վրա սեղմումը, ուստի եթե սեսիան դա չի թույլատրում, այն մերժվում է։ Հեռակառավարման շարժումները, ինչպես վահանակում, չեն չեղարկվում (undo)։

Միացրեք դրանք Preferences › REMOTE CONTROL բաժնում։ Մինչև ընտրելը երկուսն էլ անջատված են։

**OSC**-ը լսում է 8000 պորտը.
- This PC. միայն այս համակարգչի հավելվածները։
- Network. նաև ձեր ցանցի պլանշետները։ OSC-ն գաղտնաբառ չունի, ուստի ցանցում պորտը իմացող ցանկացած մեկը կարող է կառավարել շոուն։

Ֆեյդերներին և մաստերներին ուղարկեք 0-ից 1 արժեք. կոճակներն աշխատում են 1-ի դեպքում (սեղմում) և անտեսում են 0-ն (բաց թողնում).
- /stagecraft/cue/go, /stagecraft/cue/back, /stagecraft/cue/goto <cue համար>
- /stagecraft/playback/1/level … /stagecraft/playback/10/level
- /stagecraft/playback/N/flash (1՝ սեղմված, 0՝ բաց), /stagecraft/playback/N/go, /stagecraft/playback/N/release
- /stagecraft/master/grand, /stagecraft/master/blackout (1՝ միացված, 0՝ անջատված), /stagecraft/master/audio

Վահանակը պատասխանում է վերջին ուղարկած սարքի 9000 պորտին, որպեսզի պլանշետի ֆեյդերները հետևեն վահանակին՝ /stagecraft/cue/active, playback-ների մակարդակները և մաստերները։

**MIDI**. ընտրեք ձեր կոնտրոլերը MIDI input տողում (միացրեք այն մինչև Preferences-ը բացելը)։ Լռելյայն քարտեզը համապատասխանում է Korg nanoKONTROL2-ին՝ 1–8 ֆեյդերները 1–8 playback-ներն են, S-ը flash է, M-ը playback GO է, R-ը release է, 1-ին պտուտակը Grand Master-ն է, 2-րդը՝ Audio master-ը, ▶-ը GO է, ◀◀-ը՝ BACK, ●-ը՝ Blackout։

Ընտրությունների տակ գտնվող կարգավիճակի տողը ցույց է տալիս վերջին ստացված հաղորդագրությունը, որպեսզի ստուգեք, թե որ կառավարիչը ինչ է ուղարկում։
