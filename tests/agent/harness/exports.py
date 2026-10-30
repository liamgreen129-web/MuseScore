# SPDX-License-Identifier: GPL-3.0-only
# MuseScore Agent harness: export normalisation and comparison.
#
# What differs between any two saves of the same score, and is removed before comparing:
#   musicxml  text, <encoding-date> removed
#   mxl       every zip member, the score XML normalised as above
#   midi      bytes
#   mscz      every zip member except thumbnails, audio and view settings (window state,
#             not score); the main .mscx compared as score.mscx whatever it was saved as;
#             <programRevision> (the build's git hash) blanked in every .mscx
import io
import re
import zipfile

MSCZ_IGNORED = re.compile(r"(^Thumbnails/|audiosettings\.json$|viewsettings\.json$)")
FORMAT_SUFFIX = {"musicxml": "musicxml", "mxl": "mxl", "midi": "mid", "mscz": "mscz", "pdf": "pdf"}


def norm_xml(data):
    """Remove what changes between saves of the same score: the MusicXML encoding date, and
    the build's git revision that every .mscx records (a rebuild must not break the goldens)."""
    data = re.sub(rb"<encoding-date>[^<]*</encoding-date>", b"", data)
    return re.sub(rb"<programRevision>[^<]*</programRevision>", b"<programRevision/>", data)


def _rootfile_ignored(line, ignored):
    m = re.search(rb"""full-path\s*=\s*["']([^"']*)["']""", line)
    return bool(m) and bool(ignored.search(m.group(1).decode("utf-8", "replace")))


def norm_golden(data):
    """norm_xml, and element ids blanked: MuseScore gives every newly created element a random
    EID (and part copies name it in <linkedTo>), so a golden made by one run never matches the next. Goldens only; within a run (undo,
    redo, unchanged-after-error) ids are compared as they are."""
    data = re.sub(rb"<eid>[^<]*</eid>", b"<eid/>", norm_xml(data))
    # the MusicXML export stamps the day it runs when the score has no creation date
    data = re.sub(rb'(<miscellaneous-field name="creationDate">)[^<]*(</miscellaneous-field>)', rb"\1\2", data)
    return re.sub(rb"<linkedTo>[^<]*</linkedTo>", b"<linkedTo/>", data)


def zip_members(data, ignored=None):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        members = {n: z.read(n) for n in z.namelist() if not (ignored and ignored.search(n))}
    # the main .mscx is named after the file it was saved as; compare it as score.mscx
    main = [n for n in members if "/" not in n and n.endswith(".mscx")]
    if len(main) == 1 and main[0] != "score.mscx":
        old = main[0]
        members["score.mscx"] = members.pop(old)
        for n in members:
            if n.endswith("container.xml"):
                members[n] = members[n].replace(old.encode(), b"score.mscx")
    # and the container lists the ignored members only when they were written
    if ignored:
        for n in members:
            if n.endswith("container.xml"):
                members[n] = b"\n".join(line for line in members[n].split(b"\n")
                                        if not _rootfile_ignored(line, ignored))
    return members


def normalise(fmt, data, golden=False):
    """A stable byte form of an export, for writing goldens and hashing.

    musicxml: encoding-date removed. mxl/mscz: a deterministic zip of the normalised members
    (sorted, fixed timestamps), so a golden never changes because of the time it was made.
    midi and anything else: unchanged."""
    norm = norm_golden if golden else norm_xml
    if fmt == "musicxml":
        return norm(data)
    if fmt in ("mxl", "mscz"):
        members = zip_members(data, MSCZ_IGNORED if fmt == "mscz" else None)
        out = io.BytesIO()
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for n in sorted(members):
                x = members[n]
                if n.endswith((".xml", ".musicxml", ".mscx")):
                    x = norm(x)
                info = zipfile.ZipInfo(n, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, x)
        return out.getvalue()
    return data


def compare(fmt, ref, got, ref_name="reference", got_name="export", golden=False):
    """Return '' when equal after normalisation, else a short description of the first difference.
    golden=True also ignores element ids (see norm_golden)."""
    norm = norm_golden if golden else norm_xml
    if fmt == "musicxml":
        a, b = norm(ref), norm(got)
    elif fmt in ("mxl", "mscz"):
        ra = zip_members(ref, MSCZ_IGNORED if fmt == "mscz" else None)
        rb = zip_members(got, MSCZ_IGNORED if fmt == "mscz" else None)
        if set(ra) != set(rb):
            return (f"members differ: only in {ref_name} {sorted(set(ra) - set(rb))}, "
                    f"only in {got_name} {sorted(set(rb) - set(ra))}")
        for n in sorted(ra):
            x, y = ra[n], rb[n]
            if n.endswith((".xml", ".musicxml", ".mscx")):
                x, y = norm(x), norm(y)
            if x != y:
                return f"{n}: {first_diff(x, y, ref_name, got_name)}"
        return ""
    else:
        a, b = ref, got
    return "" if a == b else first_diff(a, b, ref_name, got_name)


def first_diff(a, b, ref_name="UI", got_name="export"):
    la, lb = a.splitlines(), b.splitlines()
    for i, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return f"line {i + 1}: {ref_name} {x[:120]!r} vs {got_name} {y[:120]!r}"
    return f"lengths {len(a)} vs {len(b)}"
