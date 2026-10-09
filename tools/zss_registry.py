# SPDX-License-Identifier: GPL-2.0-only
"""Reads the Vulkan registry (vk.xml) into plain Python data.

Used by the generators. Only the desktop Vulkan API is read; Vulkan SC,
provisional extensions and platforms this project does not build for are left
out.
"""
import os
import xml.etree.ElementTree as ET

PLATFORMS = {None: None, "xcb": "VK_USE_PLATFORM_XCB_KHR", "xlib": "VK_USE_PLATFORM_XLIB_KHR",
             "wayland": "VK_USE_PLATFORM_WAYLAND_KHR"}
DISPATCHABLE = {"VkInstance", "VkPhysicalDevice", "VkDevice", "VkQueue", "VkCommandBuffer"}
INSTANCE_LEVEL = {"VkInstance", "VkPhysicalDevice"}


def find_registry(hint=None):
    here = os.path.dirname(os.path.abspath(__file__))
    # The build passes the registry of the headers it compiles against. Without a
    # hint, prefer that same bundled copy, so that a hand run matches the build.
    candidates = [hint]
    sub = os.path.join(here, "..", "subprojects")
    if os.path.isdir(sub):
        candidates += [os.path.join(sub, d, "registry", "vk.xml") for d in sorted(os.listdir(sub), reverse=True)]
    candidates.append("/usr/share/vulkan/registry/vk.xml")
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise SystemExit("vk.xml not found; pass --registry")


def _for_vulkan(elem):
    api = elem.get("api")
    return api is None or "vulkan" in api.split(",")


def _text(elem):
    return " ".join("".join(elem.itertext()).split())


class Command:
    def __init__(self, name, ret, params):
        self.name, self.ret, self.params = name, ret, params  # params: [(declaration, name, type)]
        self.since = None       # "1.0", "1.1", ... for core commands
        self.extensions = []    # names of the extensions that provide it
        self.alias_of = None

    @property
    def first_type(self):
        return self.params[0][2] if self.params else None

    @property
    def level(self):
        """'global', 'instance' or 'device', by what the first argument dispatches on."""
        if self.first_type not in DISPATCHABLE:
            return "global"
        return "instance" if self.first_type in INSTANCE_LEVEL else "device"


class Registry:
    def __init__(self, path=None):
        self.path = find_registry(path)
        root = ET.parse(self.path).getroot()
        self.commands = {}
        self.extensions = {}     # name -> {"number", "type", "promotedto", "platform", "commands"}
        self.versions = {}       # "1.2" -> [command names]
        self.enum_values = {}    # "VK_STRUCTURE_TYPE_..." -> int
        self.structs = {}        # name -> {"stype", "extends", "members": [(type, name, text)], "lens": {name: counter}}
        self._read_commands(root)
        self._read_enums(root)
        self._read_features(root)
        self._read_extensions(root)
        self._read_structs(root)

    def _read_commands(self, root):
        aliases = []
        for c in root.find("commands"):
            if c.tag != "command" or not _for_vulkan(c):
                continue
            if c.get("alias"):
                aliases.append((c.get("name"), c.get("alias")))
                continue
            proto = c.find("proto")
            name = proto.find("name").text
            ret = _text(proto)[: -len(name)].strip()
            params = []
            for p in c.findall("param"):
                if _for_vulkan(p):
                    params.append((_text(p), p.find("name").text, p.find("type").text))
            self.commands[name] = Command(name, ret, params)
        for name, target in aliases:
            t = self.commands.get(target)
            if t:
                a = Command(name, t.ret, t.params)
                a.alias_of = target
                self.commands[name] = a

    def _read_enums(self, root):
        for e in root.findall("enums"):
            for v in e.findall("enum"):
                if v.get("value") and _for_vulkan(v):
                    try:
                        self.enum_values[v.get("name")] = int(v.get("value"), 0)
                    except ValueError:
                        pass

    def _extension_enum(self, e, extnumber):
        if e.get("value"):
            try:
                return int(e.get("value"), 0)
            except ValueError:
                return None
        if e.get("offset"):
            number = int(e.get("extnumber") or extnumber)
            value = 1000000000 + (number - 1) * 1000 + int(e.get("offset"))
            return -value if e.get("dir") == "-" else value
        return None

    def _read_features(self, root):
        for f in root.findall("feature"):
            if not _for_vulkan(f):
                continue
            number = f.get("number")
            for req in f.findall("require"):
                if not _for_vulkan(req):
                    continue
                for c in req.findall("command"):
                    if c.get("name") in self.commands:
                        self.commands[c.get("name")].since = self.commands[c.get("name")].since or number
                        self.versions.setdefault(number, []).append(c.get("name"))
                for e in req.findall("enum"):
                    v = self._extension_enum(e, e.get("extnumber") or 0)
                    if v is not None and e.get("extends"):
                        self.enum_values[e.get("name")] = v

    def _read_extensions(self, root):
        for x in root.find("extensions"):
            supported = (x.get("supported") or "").split(",")
            if "vulkan" not in supported or x.get("platform") not in PLATFORMS:
                continue
            name = x.get("name")
            info = {"number": int(x.get("number")), "type": x.get("type"), "promotedto": x.get("promotedto"),
                    "platform": x.get("platform"), "commands": []}
            for req in x.findall("require"):
                if not _for_vulkan(req):
                    continue
                for c in req.findall("command"):
                    if c.get("name") in self.commands and c.get("name") not in info["commands"]:
                        info["commands"].append(c.get("name"))
                        self.commands[c.get("name")].extensions.append(name)
                for e in req.findall("enum"):
                    v = self._extension_enum(e, x.get("number"))
                    if v is not None and e.get("extends"):
                        self.enum_values[e.get("name")] = v
            self.extensions[name] = info

    def _read_structs(self, root):
        for t in root.find("types"):
            if t.get("category") != "struct" or not _for_vulkan(t) or t.get("alias"):
                continue
            members, stype, lens = [], None, {}
            for m in t.findall("member"):
                if not _for_vulkan(m):
                    continue
                members.append((m.find("type").text, m.find("name").text, _text(m)))
                if m.get("len"):
                    lens[m.find("name").text] = m.get("len")
                if m.find("name").text == "sType" and m.get("values"):
                    stype = m.get("values").split(",")[0]
            self.structs[t.get("name")] = {"stype": stype, "extends": (t.get("structextends") or "").split(","),
                                           "members": members, "lens": lens}

    def buildable(self):
        """Commands that exist in a build for this project's platforms: core, or from a kept extension."""
        return sorted(n for n, c in self.commands.items() if c.since or c.extensions)

    def guard(self, command):
        """Preprocessor symbol a command needs, or None."""
        c = self.commands[command]
        if c.since:
            return None
        guards = {PLATFORMS[self.extensions[e]["platform"]] for e in c.extensions}
        return None if None in guards else sorted(guards)[0]

    def feature_structs(self):
        """Structures made only of VkBool32 switches that an application chains to enable features."""
        out = {}
        for name, s in self.structs.items():
            if not s["stype"] or s["stype"] not in self.enum_values:
                continue
            if not ({"VkDeviceCreateInfo", "VkPhysicalDeviceFeatures2"} & set(s["extends"])):
                continue
            rest = [m for m in s["members"] if m[1] not in ("sType", "pNext")]
            if rest and all(m[0] == "VkBool32" and "[" not in m[2] for m in rest):
                out[name] = [m[1] for m in rest]
        return out


if __name__ == "__main__":
    r = Registry()
    print(f"{r.path}: {len(r.commands)} commands, {len(r.extensions)} extensions, {len(r.structs)} structures")
    for v in sorted(r.versions):
        print(f"  Vulkan {v}: {len(set(r.versions[v]))} commands")
    print(f"  buildable here: {len(r.buildable())} commands; feature structures: {len(r.feature_structs())}")
