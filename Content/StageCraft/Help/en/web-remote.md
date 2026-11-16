# Web Remote: running the desk from a phone or tablet
keywords: web remote, phone, tablet, browser, mobile, pin, pair, pairing, remote control, wifi, ipad, android, iphone, http, qr, qr code, scan, camera, remember, forget, unpair
panel: StageCraft.Panel.Preferences

The Web Remote lets a phone or tablet run the desk from its browser, with no app to install: cue GO and BACK, the cue list, the ten playbacks (level, flash, GO, release), the Grand Master, Blackout and the Audio master. Each move goes through the same rules as a click on the desk, so a session that does not allow it refuses it, and the phone shows why. Remote moves are not undoable, like the desk.

- Turn it on in Preferences > WEB REMOTE. This PC serves only a browser on this computer; Network serves phones and tablets on the same network. The phone and the PC must be on the same Wi-Fi or wired network.
- The quickest way: with Network on, Preferences shows a QR code. Point the phone's camera at it: the page opens and pairs by itself. The code holds the address and the PIN, so anyone who can see your screen can pair the same way; NEW PIN makes a new code.
- If the phone cannot open the page, the PC may have several network cards (VPN, virtual machines): OTHER ADDRESS shows the code for the next address.
- Or open the address Preferences shows (for example http://192.168.1.20:8080) on the phone and enter the 6-digit PIN. Use the address with the numbers: a name for the PC is refused, so another website cannot reach the remote.
- With Remember devices On (the default), a paired phone stays paired when StageCraft restarts. A phone not used for 30 days is forgotten. Preferences lists the paired devices; FORGET disconnects one of them, and Unpair on the phone's page forgets it too. With Remember devices Off, every phone pairs again after a restart. StageCraft keeps only a fingerprint of each phone's key, never the key itself.
- The page reconnects on its own when the Wi-Fi drops.
- NEW PIN changes the PIN for new phones; phones already paired stay. UNPAIR ALL disconnects every phone, remembered ones too; each must pair again with the new PIN.
- A device that enters five wrong PINs in a row must wait 30 seconds before it can try again. After many wrong PINs StageCraft also makes a new PIN.
- The faders on the phone follow the desk, and the desk follows the phone. Up to eight phones can be paired, and they all see the same show.
- The connection is plain HTTP, not encrypted: use it on a network you trust, as with OSC. Anyone who can read the network's traffic could see the PIN.
- Windows may ask whether StageCraft may use the network the first time you choose Network. Allow private networks.
- Scripts and stream-deck style buttons can send the same commands: pair with POST /api/pair, then POST /api/command with the token and a path from Help: MIDI and OSC, for example {"path":"/stagecraft/cue/go"}.
- The Web Remote is part of StageCraft Pro. Without a licence every command is refused, and the phone says why (Help: StageCraft Pro).
