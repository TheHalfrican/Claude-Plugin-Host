"""Bridge commands against fake Live objects."""
import json

import pytest

from ClaudeBridge.commands import Commands, parse_quantity, value_for_text, CommandError
import fake_live


@pytest.fixture
def song():
    return fake_live.make_set()


@pytest.fixture
def run(song):
    commands = Commands(lambda: song, fake_live.App())
    return lambda **request: commands.run(request)


def ok(reply):
    assert reply["ok"], reply.get("error")
    return reply


def err(reply):
    assert not reply["ok"], reply
    return reply["error"]


# --- basics ------------------------------------------------------------------

def test_info_reports_versions_and_commands(run):
    r = ok(run(cmd="info"))
    assert r["live"] == "12.2.7" and r["tracks"] == 5 and r["returns"] == 2
    assert "set_sidechain" in r["commands"] and "envelope_set" in r["commands"]


def test_unknown_commands_list_the_real_ones(run):
    assert "set_track" in err(run(cmd="explode"))
    assert "unknown cmd" in err(run(cmd="../etc"))


def test_tracks_lists_regular_return_and_master(run):
    tracks = ok(run(cmd="tracks"))["tracks"]
    assert [t["kind"] for t in tracks] == ["track"] * 5 + ["return"] * 2 + ["master"]
    keys = tracks[2]
    assert keys["name"] == "Keys" and keys["volume"] == "0.0 dB" and keys["pan"] == "C"
    assert keys["sends"][0] == {"to": "A-Reverb", "text": "-inf dB"}
    assert keys["output"] == "Main"


# --- refs --------------------------------------------------------------------

@pytest.mark.parametrize("key, expected", [
    ("Keys", "Keys"), ("keys", "Keys"), (2, "Keys"),
    ("Bass (Host)", "Bass (Host)"),        # exact wins over the partial "Bass (Serum)"
    ("Kick", "Kick (SC)"),                 # unique partial
    ("A-Reverb", "A-Reverb"), ("Delay", "B-Delay"),
    ("Master", "Master"), ("main", "Master"),
])
def test_track_refs(run, key, expected):
    assert ok(run(cmd="track", track=key))["track"]["name"] == expected


def test_ambiguous_and_missing_tracks_explain_themselves(run):
    assert "matches several" in err(run(cmd="track", track="Bass"))
    assert "no track named" in err(run(cmd="track", track="Vocals"))
    assert "out of range" in err(run(cmd="track", track=40))


# --- mixer -------------------------------------------------------------------

def test_set_track_flags_volume_pan_and_sends(run, song):
    r = ok(run(cmd="set_track", track="Kick (SC)", mute=True, volume="-6 dB", pan="25L",
               sends={"A": "-12 dB", "Delay": "-18 dB"}))
    kick = song.tracks[4]
    assert kick.mute is True
    assert r["changed"]["volume"] == "-6.0 dB"
    assert r["changed"]["pan"] == "25L"
    assert r["changed"]["send A"] == "-12.0 dB"
    assert r["changed"]["send Delay"] == "-18.0 dB"


def test_pan_center_and_right(run):
    assert ok(run(cmd="set_track", track="Keys", pan="C"))["changed"]["pan"] == "C"
    assert ok(run(cmd="set_track", track="Keys", pan="40R"))["changed"]["pan"] == "40R"


def test_volume_can_go_to_minus_infinity(run):
    assert ok(run(cmd="set_track", track="Keys", volume="-inf dB"))["changed"]["volume"] == "-inf dB"


def test_set_track_refuses_nonsense(run):
    assert "nothing to change" in err(run(cmd="set_track", track="Keys"))
    assert "can't be muted" in err(run(cmd="set_track", track="Master", mute=True))
    assert "outside" in err(run(cmd="set_track", track="Keys", volume="+40 dB"))
    assert "no send" in err(run(cmd="set_track", track="Keys", sends={"Z": "-6 dB"}))


# --- routing -----------------------------------------------------------------

def test_output_to_sends_only(run, song):
    r = ok(run(cmd="set_routing", track="Kick (SC)", direction="output", type="Sends Only"))
    assert r["output"]["type"] == "Sends Only"
    assert song.tracks[4].output_routing_type.display_name == "Sends Only"


def test_routing_errors(run):
    assert "direction" in err(run(cmd="set_routing", track="Keys", direction="sideways", type="Main"))
    assert "no routing named" in err(run(cmd="set_routing", track="Keys", direction="output", type="Nowhere"))


def test_sidechain_source_and_tap(run, song):
    r = ok(run(cmd="set_sidechain", track="Bass (Host)", device="Claude Host FX", source="Kick (SC)", tap="Post FX"))
    assert r["source"] == "Kick (SC)" and r["tap"] == "Post FX"
    io = song.tracks[1].devices[1].input_routings[0]
    assert io.routing_type.display_name == "Kick (SC)"


def test_sidechain_needs_a_device_with_one(run):
    assert "no sidechain input" in err(run(cmd="set_sidechain", track="Keys", device="Auto Filter", source="Drums"))


def test_devices_show_sidechain_state(run):
    devices = ok(run(cmd="devices", track="Bass (Host)"))["devices"]
    assert devices[1]["sidechain"] == [{"source": "No Input", "tap": "Pre FX"}]
    assert "sidechain" not in devices[0]


# --- devices and tracks --------------------------------------------------------

def test_set_param_by_choice_text_and_raw(run, song):
    assert ok(run(cmd="set_param", track="Keys", device="Auto Filter", param="Device On", text="Off"))["param"]["text"] == "Off"
    r = ok(run(cmd="set_param", track="Keys", device="Auto Filter", param="Frequency", text="1000 Hz"))
    assert abs(float(r["param"]["text"].split()[0]) - 1000) <= 20
    assert "outside" in err(run(cmd="set_param", track="Keys", device="Auto Filter", param="Frequency", value=3.0))
    assert "no choice" in err(run(cmd="set_param", track="Keys", device="Auto Filter", param="Device On", text="Maybe"))


def test_params_lists_device_and_mixer(run):
    names = [p["name"] for p in ok(run(cmd="params", track="Keys", device="Auto Filter"))["params"]]
    assert names == ["Device On", "Frequency"]
    mixer = [p["name"] for p in ok(run(cmd="params", track="Keys", device="mixer"))["params"]]
    assert mixer[:2] == ["Track Volume", "Track Panning"]


def test_duplicate_delete_track_and_device(run, song):
    r = ok(run(cmd="duplicate_track", track="Drums"))
    assert r["new_index"] == 4 and len(song.tracks) == 6
    assert ok(run(cmd="delete_track", track=4))["deleted"] == "Drums"
    assert len(song.tracks) == 5
    r = ok(run(cmd="delete_device", track="Keys", device="Auto Filter"))
    assert r["devices"] == ["Electric"]
    assert "can't be deleted" in err(run(cmd="delete_track", track="Master"))
    assert "only regular tracks" in err(run(cmd="duplicate_track", track="A-Reverb"))


# --- clip automation ----------------------------------------------------------

def test_envelope_ramp_writes_steps_and_reads_back(run, song):
    r = ok(run(cmd="envelope_set", track="Keys", clip=0, device="mixer", param="volume",
               points=[[0, "-24 dB"], [4, "0 dB"]], resolution=0.5))
    assert r["steps"] == 9  # 8 ramp steps + the final point
    samples = ok(run(cmd="envelope_get", track="Keys", clip=0, device="mixer", param="volume",
                     **{"from": 0, "to": 4, "resolution": 2}))["samples"]
    texts = [s[2] for s in samples]
    assert texts[0] == "-24.0 dB" and texts[-1] == "0.0 dB"
    assert samples[0][1] < samples[1][1] < samples[2][1]   # rising


def test_envelope_steps_and_replacing(run, song):
    args = dict(cmd="envelope_set", track="Keys", clip=0, device="Auto Filter", param="Device On")
    ok(run(points=[[0, "On"], [2, "Off"], [4, "On"]], shape="step", **args))
    env = song.tracks[2].clip_slots[0].clip.automation_envelope(song.tracks[2].devices[1].parameters[0])
    assert [round(e[2]) for e in env.events] == [1, 0, 1]
    ok(run(points=[[0, "Off"], [4, "Off"]], shape="step", **args))   # replaces, not adds
    assert all(e[2] == 0 for e in env.events)


def test_envelope_clear_and_errors(run, song):
    base = dict(track="Keys", clip=0, device="mixer", param="pan")
    ok(run(cmd="envelope_set", points=[[0, "50L"], [2, "50R"]], **base))
    ok(run(cmd="envelope_clear", **base))
    assert ok(run(cmd="envelope_get", **base))["envelope"] is None
    assert "empty" in err(run(cmd="envelope_set", track="Keys", clip=3, device="mixer", param="pan", points=[[0, "C"]]))
    assert "give \"clip\"" in err(run(cmd="envelope_set", track="Keys", device="mixer", param="pan", points=[[0, "C"]]))


# --- text values ---------------------------------------------------------------

@pytest.mark.parametrize("text, value", [
    ("-6 dB", -6.0), ("2.5 kHz", 2500.0), ("250 ms", 250.0), ("1.2 s", 1200.0),
    ("25L", -25.0), ("25R", 25.0), ("4:1", 4.0), ("4.00:1", 4.0), ("1e3 Hz", 1000.0),
])
def test_parse_quantity(text, value):
    assert parse_quantity(text)[0] == pytest.approx(value)


@pytest.mark.parametrize("text", ["C", "On", "1/4", "Post FX", ""])
def test_words_are_not_numbers(text):
    assert parse_quantity(text) is None


# --- feeding a sidechain by routing ---------------------------------------------

def test_feed_sidechain_routes_the_key_into_the_device(run, song):
    r = ok(run(cmd="feed_sidechain", **{"from": "Kick (SC)", "to": "Bass (Host)", "device": "Claude Host FX"}))
    assert r["channel"] == "Sidechain-Claude Host FX"
    kick = song.tracks[4]
    assert kick.output_routing_type.display_name == "Bass (Host)"
    assert kick.output_routing_channel.display_name == "Sidechain-Claude Host FX"


def test_feed_sidechain_without_a_device_name_takes_the_only_one(run):
    assert ok(run(cmd="feed_sidechain", **{"from": "Kick (SC)", "to": "Bass (Host)"}))["channel"] == "Sidechain-Claude Host FX"


def test_feed_sidechain_to_a_track_without_one_puts_routing_back(run, song):
    assert "no device with a sidechain" in err(run(cmd="feed_sidechain", **{"from": "Kick (SC)", "to": "Keys"}))
    assert song.tracks[4].output_routing_type.display_name == "Main"


def test_a_ramp_to_the_clip_end_still_arrives(run, song):
    # A delay throw over the last bar of a 16-beat clip.
    ok(run(cmd="envelope_set", track="Keys", clip=0, device="mixer", param="send B",
           points=[[12, "-inf dB"], [16, "-12 dB"]], resolution=0.5))
    samples = ok(run(cmd="envelope_get", track="Keys", clip=0, device="mixer", param="send B",
                     **{"from": 15.5, "to": 15.5}))["samples"]
    assert samples[0][2] == "-12.0 dB"


# --- naming hosts after their plugin --------------------------------------------

def test_rename_a_live_device_and_find_it_by_type_afterwards(run, song):
    assert ok(run(cmd="rename_device", track="Keys", device="Auto Filter", name="Lo-fi Filter"))["device"] == "Lo-fi Filter"
    assert ok(run(cmd="params", track="Keys", device="Auto Filter"))["params"]   # still found by type
    assert "give the new" in err(run(cmd="rename_device", track="Keys", device=1, name=" "))


def test_renaming_a_plugin_device_reports_that_live_kept_its_name(run, song):
    assert "scripts can rename Live's own devices but not plugins" in err(
        run(cmd="rename_device", track="Bass (Host)", device="Claude Host FX", name="FF Pro-C 2 (Claude Host)"))
    assert song.tracks[1].devices[1].name == "Claude Host FX"


def test_name_hosts_reports_what_live_actually_shows(run, song, tmp_path):
    (tmp_path / "a.json").write_text(json.dumps({"tag": 1000, "plugin": "FF Pro-C 2"}))
    r = ok(run(cmd="name_hosts", registry=str(tmp_path)))
    assert r["hosts"] == [{"track": "Bass (Host)", "tag": 1000, "plugin": "FF Pro-C 2",
                           "name": "Claude Host FX", "renamed": False}]


def test_name_hosts_leaves_unknown_tags_alone(run, song, tmp_path):
    assert ok(run(cmd="name_hosts", registry=str(tmp_path)))["hosts"] == []


def test_background_tick_never_raises(song):
    commands = Commands(lambda: song, fake_live.App())
    song.tracks = None   # even with a broken set
    commands.background_tick()


def test_move_device_within_and_between_tracks(run, song):
    r = ok(run(cmd="move_device", track="Keys", device="Auto Filter", to_track="Keys", position=0))
    assert r["devices"] == ["Auto Filter", "Electric"]
    r = ok(run(cmd="move_device", track="Keys", device="Auto Filter", to_track="Drums"))
    assert r["devices"] == ["Boom Bap Kit", "Auto Filter"]
    assert [d.name for d in song.tracks[2].devices] == ["Electric"]
    assert "isn't a Rack" in err(run(cmd="move_device", track="Drums", device="Auto Filter", to_track="Keys", to_device="Electric"))


def test_inspect_the_song():
    song = fake_live.make_set()
    r = Commands(lambda: song, fake_live.App()).run({"cmd": "inspect", "track": "song", "filter": "track"})
    assert r["ok"] and "duplicate_track" in r["attributes"]
