# Later list

Things deliberately parked, with what we already know about them.

## Full automation control

Goal: Claude can write, read and edit automation for anything in a set — Live devices, the mixer, and the knobs of plugins inside Claude Hosts — in both Session clips and the Arrangement, the way a person draws it.

What works today (Claude Bridge):
- **Clip automation** for any parameter Live can see (Live devices, mixer volume/pan/sends, Configured plugin knobs): write ramps or steps, read back, clear. Session and Arrangement clips both work.
- Points on a clip's last beat are pulled onto its last step, because Live drops events at a clip's end.

What's missing, and the likely way in:

1. **Automating knobs of plugins inside Claude Hosts.** Live only sees a host's "Instance Tag", so it can't automate the hosted plugin.
   - Plan: give each host a fixed set of **automation slots** (say 16 parameters "Slot 1"–"Slot 16"), visible to Live.
   - Each slot gets mapped by command to an inner parameter, with an optional range: `host_ctl.py map-slot 1 "Fil Cutoff" --from "200 Hz" --to "8 kHz"`.
   - The host forwards slot changes to the inner plugin. Mappings are saved with the set.
   - Live can then automate the slots like any device. Clip envelopes via the bridge, the user's own drawing, and recording all work.
   - Open questions: slot names in Live are fixed once the plugin loads (AU/VST3 limit), so show the mapping in the host's header. Also decide how a slot behaves while its inner parameter is also being set by command.
2. **Arrangement track automation lanes** (not inside clips). Live's API has no call to draw them.
   - Options to investigate:
     - (a) **Record** automation through the bridge: arm Arrangement automation recording, play, and move the parameter in real time from the bridge. Precise timing would need care.
     - (b) A **Max for Live** device, if its API can write Arrangement envelopes.
     - (c) Write everything as clip envelopes on Arrangement clips, which already works, and accept that it lives in clips.
3. **Reading existing automation** across a set: which parameters are automated (`automation_state`), and their values over time. This lets Claude see and respect what the user drew.
4. **Editing rather than replacing.** Insert, move and delete individual breakpoints, and use curve shapes. The bridge currently replaces a range with steps at a fixed resolution.
5. **Musical helpers on top:** filter sweeps over N bars, fades, delay/reverb throws on phrase ends, sidechain-style volume shapes, and tempo-synced LFO-like movement, all expressed in bars and beats.

## Also parked

- **Multi-out signal test in Live** with a real multi-output instrument (Kontakt or Battery with a kit). The routing is already confirmed: Live lists "Aux 1–7-Claude Host Instrument".
- **Plugin presets through Live's API:** plugin devices expose `presets` and `selected_preset_index` (seen with `bridge_ctl.py inspect`). Worth checking whether that browses AU factory presets for plugins loaded directly in Live.
- **Unmapped codes:** Pro-C 2 `Style`, Pro-Q 2 `Processing Mode` and `Processing Resolution`, Saturn `Band State`.
- **Serum 2 license:** blocked on Xfer support restoring the taken-over account.
- **Account security:** 2-step verification and forwarding-rule check on both Gmail accounts.
