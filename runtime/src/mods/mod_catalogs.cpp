// Catalog merging (mods/mod_catalogs.h).
#include "mods/mod_catalogs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include "mods/mod_registry.h"
#include "runtime_config.h"
#include "runtime_log.h"

namespace Mods::Catalogs {
namespace {

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<uint32_t>(d[o]) << 24 | static_cast<uint32_t>(d[o + 1]) << 16 | static_cast<uint32_t>(d[o + 2]) << 8 | d[o + 3];
}
void Put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = static_cast<uint8_t>(v >> 24); d[o + 1] = static_cast<uint8_t>(v >> 16);
    d[o + 2] = static_cast<uint8_t>(v >> 8); d[o + 3] = static_cast<uint8_t>(v);
}
uint32_t LowerHash(const std::string& s) {  // nlStringLowerHash
    uint32_t h = 0xFFFFFFFFu;
    for (const unsigned char c : s) h = h * 33 + static_cast<uint8_t>(std::tolower(c));
    return h;
}
std::vector<uint32_t> g_feAdded;  // hashes of the textures merged in (read-only after boot)
std::map<uint32_t, std::filesystem::path> g_feFiles;  // hash -> the .gxt it came from
std::vector<std::string> g_locAdded;  // keys merged in
std::map<std::string, std::string> g_nisTriggers;  // cutscene -> the one whose trigger script it uses
std::map<uint32_t, uint32_t> g_nisCues;            // cue hash -> the cue it plays instead

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// INI tables with a section per character (by internal name): a mod character without a section of
// its own gets a copy of its base's under its name (hologram framing, ...).
constexpr const char* kCharacterSectionInis[] = {"ini/ImpostorCharacterTweaks.ini"};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The [section] a line opens, lower-cased, or "".
std::string SectionOf(const std::string& line) {
    size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t\r");
    if (a == std::string::npos || line[a] != '[' || line[b] != ']') return {};
    return Lower(line.substr(a + 1, b - a - 1));
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

std::string Trim(const std::string& v) {
    const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
}

// The "name = value" lines of a catalog text file (UTF-8; blank lines and # comments skipped), in order.
std::vector<std::pair<std::string, std::string>> Assignments(const std::vector<uint8_t>& bytes) {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::string line : Lines(std::string(bytes.begin(), bytes.end()))) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // BOM
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        if (std::string name = Trim(line.substr(0, eq)); !name.empty()) out.emplace_back(std::move(name), Trim(line.substr(eq + 1)));
    }
    return out;
}

// The cache folder rebuilt catalog files go to: <data>/Cache/mods/<sub>.
std::filesystem::path CacheDir(const std::string& sub) {
    return RuntimeConfigFile::ApplicationDataDirectory() / "Cache" / "mods" / sub;
}

bool WriteCache(const std::filesystem::path& path, const void* data, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (stream) return true;
    RT_LOG(RT_TAG_MODS) << "catalogs: can't write " << RuntimeConfigFile::PathToUtf8(path) << std::endl;
    return false;
}

}  // namespace

std::vector<uint8_t> MergeFeBundle(const std::vector<uint8_t>& bundle,
                                   const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& additions) {
    if (bundle.size() < 0x20 || Be32(bundle, 0) != 0x20) return {};
    const uint32_t count = Be32(bundle, 4);
    if (0x20 + static_cast<size_t>(count) * 12 > bundle.size()) return {};
    struct Item { uint32_t hash; std::vector<uint8_t> data; };
    std::vector<Item> items;
    items.reserve(count + additions.size());
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = 0x20 + static_cast<size_t>(i) * 12;
        const uint32_t hash = Be32(bundle, e), offset = Be32(bundle, e + 4) * 32, size = Be32(bundle, e + 8);
        if (static_cast<size_t>(offset) + size > bundle.size()) return {};
        items.push_back({hash, std::vector<uint8_t>(bundle.begin() + offset, bundle.begin() + offset + size)});
    }
    for (const auto& [hash, data] : additions) {
        const auto it = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.hash == hash; });
        if (it != items.end()) it->data = data;
        else items.push_back({hash, data});
    }
    const size_t table = 0x20 + items.size() * 12;
    const size_t start = (table + 31) & ~static_cast<size_t>(31);
    std::vector<uint8_t> out(bundle.begin(), bundle.begin() + 0x20);  // header (its other fields kept)
    out.resize(start, 0);
    Put32(out, 4, static_cast<uint32_t>(items.size()));
    Put32(out, 12, static_cast<uint32_t>(start / 32));
    for (size_t i = 0; i < items.size(); ++i) {
        const size_t offset = out.size();
        Put32(out, 0x20 + i * 12, items[i].hash);
        Put32(out, 0x20 + i * 12 + 4, static_cast<uint32_t>(offset / 32));
        Put32(out, 0x20 + i * 12 + 8, static_cast<uint32_t>(items[i].data.size()));
        out.insert(out.end(), items[i].data.begin(), items[i].data.end());
        out.resize((out.size() + 31) & ~static_cast<size_t>(31), 0);
    }
    return out;
}

namespace {
// An NL chunk, losslessly: its raw id (alignment bits kept) and its data or children.
struct Chunk {
    uint32_t rawId = 0;
    std::vector<uint8_t> data;
    std::vector<Chunk> children;
    uint32_t Id() const { return rawId & 0x80FFFFFFu; }
    bool Container() const { return (rawId & 0x80000000u) != 0; }
};

bool ParseChunks(const std::vector<uint8_t>& d, size_t off, size_t end, std::vector<Chunk>& out, int depth = 0) {
    if (depth > 8) return false;
    while (off + 8 <= end) {
        Chunk c;
        c.rawId = Be32(d, off);
        const size_t chunkEnd = off + 8 + Be32(d, off + 4);
        if (chunkEnd > end) return false;
        size_t payload = off + 8;
        if (const uint32_t al = (c.rawId >> 24) & 0xF) payload = (payload + (size_t{1} << al) - 1) & ~((size_t{1} << al) - 1);
        if (c.Container()) {
            if (!ParseChunks(d, off + 8, chunkEnd, c.children, depth + 1)) return false;
        } else {
            if (payload > chunkEnd) return false;
            c.data.assign(d.begin() + static_cast<std::ptrdiff_t>(payload), d.begin() + static_cast<std::ptrdiff_t>(chunkEnd));
        }
        out.push_back(std::move(c));
        off = chunkEnd + ((4 - (chunkEnd & 3)) & 3);
    }
    return true;
}

void SerializeChunk(const Chunk& c, std::vector<uint8_t>& out) {
    const size_t at = out.size();
    const uint32_t al = (c.rawId >> 24) & 0xF;
    const size_t pad = al ? (((size_t{1} << al) - ((at + 8) & ((size_t{1} << al) - 1))) & ((size_t{1} << al) - 1)) : 0;
    out.resize(at + 8 + pad, 0);
    const size_t body = out.size();
    if (c.Container()) {
        for (size_t i = 0; i < c.children.size(); ++i) {
            SerializeChunk(c.children[i], out);
            if (i + 1 < c.children.size()) out.resize((out.size() + 3) & ~size_t{3}, 0);
        }
    } else {
        out.insert(out.end(), c.data.begin(), c.data.end());
    }
    Put32(out, at, c.rawId);
    Put32(out, at + 4, static_cast<uint32_t>(pad + out.size() - body));
}

const Chunk* FindChild(const Chunk& parent, uint32_t id) {
    for (const Chunk& c : parent.children)
        if (c.Id() == id) return &c;
    return nullptr;
}

void Append32(std::vector<uint8_t>& d, uint32_t v) {
    const size_t at = d.size();
    d.resize(at + 4);
    Put32(d, at, v);
}
} // namespace

bool MergeStreamBank(const std::vector<uint8_t>& resbun, uint32_t nlxwbSize, const std::vector<NewStream>& streams,
                     const std::string& templateCue, std::vector<uint8_t>& merged, std::vector<uint8_t>& append,
                     std::string& error) {
    constexpr uint32_t kCueSize = 40, kVoiceSize = 44, kSeqSize = 12, kEventSize = 48, kSourceSize = 28;
    std::vector<Chunk> top;
    if (!ParseChunks(resbun, 0, resbun.size(), top) || top.size() != 1 || top[0].rawId != 0x80000001u) {
        error = "not a sound bank";
        return false;
    }
    {
        std::vector<uint8_t> again;
        SerializeChunk(top[0], again);
        if (again != resbun) {
            error = "the bank doesn't re-serialize identically";
            return false;
        }
    }
    Chunk& root = top[0];
    Chunk* smap = nullptr;
    Chunk* bundle = nullptr;
    Chunk* sources = nullptr;
    for (Chunk& c : root.children) {
        if (c.Id() == 0x80023000u) smap = &c;
        else if (c.Id() == 0x80023300u) bundle = &c;
        else if (c.Id() == 0x80023200u) sources = &c;
        else if (c.Id() == 0x23703u) {
            error = "a resident bank (it has a sample table), not a stream bank";
            return false;
        }
    }
    if (!smap || !bundle || !sources || smap->children.size() != 2 || sources->children.size() != 2 || bundle->children.size() < 7 ||
        bundle->children[0].data.size() < 64) {
        error = "not a stream bank";
        return false;
    }
    Chunk& smHeader = smap->children[0];
    Chunk& smCues = smap->children[1];
    std::vector<uint8_t>& h = bundle->children[0].data;  // ResourceBundle header: counts and base pointers
    uint32_t cues = Be32(h, 8), voices = Be32(h, 16), seqs = Be32(h, 24), events = Be32(h, 32);
    const uint32_t voiceBase = Be32(h, 20), seqBase = Be32(h, 28), eventBase = Be32(h, 36);
    std::vector<Chunk>& kids = bundle->children;
    const size_t restFirst = 7, voiceFirst = restFirst + cues, seqFirst = voiceFirst + 2 * size_t{voices},
                 eventFirst = seqFirst + seqs;
    if (kids.size() != eventFirst + events || Be32(smHeader.data, 0) * 20 != smCues.data.size()) {
        error = "unexpected resource bundle layout";
        return false;
    }
    const auto record = [&](uint32_t array, uint32_t size, uint32_t index) -> std::vector<uint8_t> {
        const std::vector<uint8_t>& a = kids[1 + array].data;
        if (size_t{index + 1} * size > a.size()) return {};
        return {a.begin() + static_cast<std::ptrdiff_t>(index * size), a.begin() + static_cast<std::ptrdiff_t>((index + 1) * size)};
    };
    // The template cue's chain: cue -> its first voice -> sequence -> sound event (-> choice).
    uint32_t templateIndex = smCues.data.empty() ? ~0u : Be32(smCues.data, 16);
    if (!templateCue.empty()) {
        templateIndex = ~0u;
        for (size_t i = 0; i + 20 <= smCues.data.size(); i += 20)
            if (Be32(smCues.data, i) == LowerHash(templateCue)) templateIndex = Be32(smCues.data, i + 16);
    }
    if (templateIndex >= cues || kids[restFirst + templateIndex].data.size() < 20) {
        error = "template cue " + templateCue + " not in the bank";
        return false;
    }
    const std::vector<uint8_t> cueT = record(0, kCueSize, templateIndex), entryT = kids[restFirst + templateIndex].data;
    const uint32_t v0 = (Be32(entryT, 0) - voiceBase) / kVoiceSize;
    if (v0 >= voices) {
        error = "template cue has no voice";
        return false;
    }
    const std::vector<uint8_t> voiceT = record(1, kVoiceSize, v0), rpcT = kids[voiceFirst + 2 * v0 + 1].data;
    const uint32_t s0 = (Be32(kids[voiceFirst + 2 * v0].data, 0) - seqBase) / kSeqSize;
    if (s0 >= seqs || kids[seqFirst + s0].data.size() < 8) {
        error = "template cue has no sequence";
        return false;
    }
    const std::vector<uint8_t> seqT = record(2, kSeqSize, s0);
    const uint32_t e0 = (Be32(kids[seqFirst + s0].data, 4) - eventBase) / kEventSize;
    if (e0 >= events || kids[eventFirst + e0].data.size() < 8 || cueT.empty() || voiceT.empty() || seqT.empty()) {
        error = "template cue has no sound event";
        return false;
    }
    const std::vector<uint8_t> eventT = record(3, kEventSize, e0), choiceT = kids[eventFirst + e0].data;
    std::vector<uint8_t>& sourceHeader = sources->children[0].data;
    std::vector<uint8_t>& sourceList = sources->children[1].data;
    if (sourceHeader.size() < 9 || sourceList.size() != size_t{Be32(sourceHeader, 0)} * kSourceSize) {
        error = "unexpected source table";
        return false;
    }
    const uint32_t interleave = Be32(sourceHeader, 4);

    std::set<uint32_t> known;
    for (size_t i = 0; i + 20 <= smCues.data.size(); i += 20) known.insert(Be32(smCues.data, i));
    append.assign((32 - nlxwbSize % 32) % 32, 0);
    uint32_t sourceCount = Be32(sourceHeader, 0);
    std::vector<Chunk> newEntries, newVoices, newSeqs, newEvents;
    for (const NewStream& stream : streams) {
        if (stream.names.empty()) continue;
        if (stream.blob.size() < 0xE0 || stream.blob.size() % 32 != 0 || Be32(stream.blob, 0) != 0x49445350u /* IDSP */ ||
            Be32(stream.blob, 4) != interleave) {
            error = stream.names[0] + ": not a stream in this bank's format (IDSP, interleave " + std::to_string(interleave) + ", 32-byte size)";
            return false;
        }
        const uint32_t offset = nlxwbSize + static_cast<uint32_t>(append.size());
        append.insert(append.end(), stream.blob.begin(), stream.blob.end());
        const uint32_t source = sourceCount++, voice = voices++, seq = seqs++, event = events++;
        const uint32_t primary = LowerHash(stream.names[0]);
        for (uint32_t v : {source, offset, static_cast<uint32_t>(stream.blob.size()), primary, 2u, 0u, 0u}) Append32(sourceList, v);
        Append32(kids[2].data, primary);  // voice record: the template's, named after the stream
        kids[2].data.insert(kids[2].data.end(), voiceT.begin() + 4, voiceT.end());
        Chunk voiceSeq{0x23309u, {}, {}}, voiceRpc{0x2330Cu, rpcT, {}};
        Append32(voiceSeq.data, seqBase + kSeqSize * seq);
        newVoices.push_back(std::move(voiceSeq));
        newVoices.push_back(std::move(voiceRpc));
        kids[3].data.insert(kids[3].data.end(), seqT.begin(), seqT.end());
        Chunk seqEvent{0x2330Au, {}, {}};
        Append32(seqEvent.data, 1);
        Append32(seqEvent.data, eventBase + kEventSize * event);
        newSeqs.push_back(std::move(seqEvent));
        kids[4].data.insert(kids[4].data.end(), eventT.begin(), eventT.end());
        Chunk choice{0x2330Bu, {}, {}};
        Append32(choice.data, source);
        choice.data.insert(choice.data.end(), choiceT.begin() + 4, choiceT.begin() + 8);
        newEvents.push_back(std::move(choice));
        for (const std::string& name : stream.names) {
            const uint32_t hash = LowerHash(name);
            if (!known.insert(hash).second) {
                error = "cue " + name + " is already in the bank";
                return false;
            }
            Append32(kids[1].data, hash);
            kids[1].data.insert(kids[1].data.end(), cueT.begin() + 4, cueT.end());
            Chunk entry{0x23308u, {}, {}};
            Append32(entry.data, voiceBase + kVoiceSize * voice);
            entry.data.insert(entry.data.end(), entryT.begin() + 4, entryT.end());
            newEntries.push_back(std::move(entry));
            for (uint32_t v : {hash, 0u, 0u, 0u, cues}) Append32(smCues.data, v);
            ++cues;
        }
    }
    Put32(smHeader.data, 0, cues);
    Put32(h, 8, cues);
    Put32(h, 16, voices);
    Put32(h, 24, seqs);
    Put32(h, 32, events);
    Put32(sourceHeader, 0, sourceCount);
    // Entry chunks in the bundle's order: cues', voices' pairs, sequences', sound events'.
    const auto at = [&](size_t i) { return kids.begin() + static_cast<std::ptrdiff_t>(i); };
    kids.insert(at(eventFirst + (events - newEvents.size())), newEvents.begin(), newEvents.end());
    kids.insert(at(eventFirst), newSeqs.begin(), newSeqs.end());
    kids.insert(at(seqFirst), newVoices.begin(), newVoices.end());
    kids.insert(at(voiceFirst), newEntries.begin(), newEntries.end());
    merged.clear();
    SerializeChunk(root, merged);
    return true;
}

namespace {

// Front-end textures: catalogs/fe/<bundle>/<path>.gxt into Art/fe/<bundle>.
int MergeFrontEnd(const HostPathOf& hostPathOf, const RegisterFile& registerFile) {
    int rebuilt = 0;
    // Front-end textures, per bundle, in load order (a later package's texture of a name wins).
    std::map<std::string, std::vector<std::pair<uint32_t, std::filesystem::path>>> fe;  // bundle file name -> textures
    std::map<std::string, std::string> owners;                                         // bundle -> packages
    for (const Package* package : ActivePackages()) {
        const std::filesystem::path root = package->root / "catalogs" / "fe";
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& bundleDir : std::filesystem::directory_iterator(root, ec)) {
            if (!bundleDir.is_directory(ec)) continue;
            const std::string bundle = RuntimeConfigFile::PathToUtf8(bundleDir.path().filename());
            for (const auto& entry : std::filesystem::recursive_directory_iterator(bundleDir.path(), ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".gxt") continue;
                std::filesystem::path relative = std::filesystem::relative(entry.path(), bundleDir.path(), ec);
                relative.replace_extension();
                std::string name = "fe/" + RuntimeConfigFile::PathToUtf8(relative);
                std::replace(name.begin(), name.end(), '\\', '/');
                fe[bundle].push_back({LowerHash(name), entry.path()});
            }
            owners[bundle] += (owners[bundle].empty() ? "" : ", ") + package->id;
        }
    }

    const std::filesystem::path cache = CacheDir("fe");
    for (const auto& [bundle, textures] : fe) {
        const std::string dvdPath = "/Art/fe/" + bundle;
        const auto host = hostPathOf(dvdPath);
        std::vector<uint8_t> original;
        if (!host || !ReadFile(*host, original)) {
            RT_LOG(RT_TAG_MODS) << "catalogs: " << dvdPath << " isn't a game file (from " << owners[bundle] << ")" << std::endl;
            continue;
        }
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> additions;
        for (const auto& [hash, path] : textures) {
            std::vector<uint8_t> data;
            if (ReadFile(path, data) && data.size() > 0x20) {
                additions.push_back({hash, std::move(data)});
                g_feAdded.push_back(hash);
                g_feFiles[hash] = path;
            }
            else RT_LOG(RT_TAG_MODS) << "catalogs: can't read " << RuntimeConfigFile::PathToUtf8(path) << std::endl;
        }
        const std::vector<uint8_t> merged = MergeFeBundle(original, additions);
        if (merged.empty()) {
            RT_LOG(RT_TAG_MODS) << "catalogs: " << dvdPath << " isn't a front-end bundle" << std::endl;
            continue;
        }
        const std::filesystem::path out = cache / bundle;
        if (!WriteCache(out, merged.data(), merged.size())) continue;
        registerFile(dvdPath, out, static_cast<uint32_t>(merged.size()));
        RT_LOG(RT_TAG_MODS) << "catalogs: " << dvdPath << " + " << additions.size() << " texture(s) from " << owners[bundle] << std::endl;
        ++rebuilt;
    }
    return rebuilt;
}

// INI files: catalogs/ini/<path>.ini appended to ini/<path>.ini, and base sections copied for
// mod characters in per-character tables.
int MergeInis(const HostPathOf& hostPathOf, const RegisterFile& registerFile) {
    int rebuilt = 0;
    // INI files: mods' fragments (<package>/catalogs/ini/<path>.ini, appended to ini/<path>.ini) and
    // the base's sections copied for mod characters in per-character tables.
    std::map<std::string, std::vector<std::filesystem::path>> fragments;  // "ini/<path>" (lower) -> files
    for (const Package* package : ActivePackages()) {
        const std::filesystem::path root = package->root / "catalogs" / "ini";
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (!entry.is_regular_file(ec) || Lower(RuntimeConfigFile::PathToUtf8(entry.path().extension())) != ".ini") continue;
            std::string relative = RuntimeConfigFile::PathToUtf8(std::filesystem::relative(entry.path(), root, ec));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            fragments[Lower("ini/" + relative)].push_back(entry.path());
        }
    }
    std::vector<std::string> targets;
    for (const auto& [file, unused] : fragments) targets.push_back(file);
    for (const char* file : kCharacterSectionInis)
        if (std::find(targets.begin(), targets.end(), Lower(file)) == targets.end()) targets.push_back(Lower(file));
    const std::filesystem::path iniCache = CacheDir("");
    for (const std::string& file : targets) {
        const auto host = hostPathOf("/" + file);
        std::vector<uint8_t> bytes;
        if (!host || !ReadFile(*host, bytes)) {
            if (fragments.count(file)) RT_LOG(RT_TAG_MODS) << "catalogs: /" << file << " isn't a game file" << std::endl;
            continue;
        }
        const std::string original(bytes.begin(), bytes.end());
        std::string merged = original;
        std::vector<std::string> sections;
        for (const std::string& line : Lines(original))
            if (std::string name = SectionOf(line); !name.empty()) sections.push_back(name);
        int added = 0;
        for (const auto& path : fragments[file]) {
            std::vector<uint8_t> text;
            if (!ReadFile(path, text)) continue;
            merged += "\r\n";
            for (const std::string& line : Lines(std::string(text.begin(), text.end()))) {
                merged += line + "\r\n";
                if (std::string name = SectionOf(line); !name.empty()) sections.push_back(name);
            }
            ++added;
        }
        if (std::any_of(std::begin(kCharacterSectionInis), std::end(kCharacterSectionInis), [&](const char* f) { return Lower(f) == file; })) {
            const std::vector<std::string> lines = Lines(original);
            for (const Package* package : ActivePackages()) {
                for (const CharacterDef& c : package->characters) {
                    if (std::find(sections.begin(), sections.end(), c.name) != sections.end()) continue;
                    const std::string base = BaseCharacterName(c.baseIndex) ? BaseCharacterName(c.baseIndex) : "";
                    bool copying = false;
                    std::string copy;
                    for (const std::string& line : lines) {
                        const std::string name = SectionOf(line);
                        if (!name.empty()) copying = name == base;
                        if (copying) copy += (name.empty() ? line : "[" + c.name + "]") + "\r\n";
                    }
                    if (copy.empty()) continue;
                    merged += "\r\n" + copy;
                    sections.push_back(c.name);
                    ++added;
                }
            }
        }
        if (added == 0) continue;
        const std::filesystem::path out = iniCache / RuntimeConfigFile::PathFromUtf8(file);
        if (!WriteCache(out, merged.data(), merged.size())) continue;
        registerFile("/" + file, out, static_cast<uint32_t>(merged.size()));
        RT_LOG(RT_TAG_MODS) << "catalogs: /" << file << " + " << added << " section group(s)" << std::endl;
        ++rebuilt;
    }
    return rebuilt;
}

// Strings: catalogs/loc/{all,<language>}.txt into Art/fe/<language>[_game].loc.
int MergeStrings(const HostPathOf& hostPathOf, const RegisterFile& registerFile) {
    int rebuilt = 0;
    // Strings.
    static const char* const kLanguages[] = {"english", "ukenglish", "french", "nafrench", "german", "italian",
                                             "spanish", "naspanish", "japanese", "bob", "longest"};
    std::map<std::string, std::map<std::string, std::string>> strings;  // language ("all") -> key -> text
    for (const Package* package : ActivePackages()) {
        const std::filesystem::path root = package->root / "catalogs" / "loc";
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (!entry.is_regular_file(ec) || Lower(RuntimeConfigFile::PathToUtf8(entry.path().extension())) != ".txt") continue;
            std::vector<uint8_t> bytes;
            if (!ReadFile(entry.path(), bytes)) continue;
            auto& table = strings[Lower(RuntimeConfigFile::PathToUtf8(entry.path().stem()))];
            for (const auto& [key, text] : Assignments(bytes)) table[key] = text;
        }
    }
    if (!strings.empty()) {
        const std::filesystem::path locCache = CacheDir("loc");
        for (const char* language : kLanguages) {
            std::map<std::string, std::string> table = strings["all"];
            for (const auto& [key, text] : strings[language]) table[key] = text;
            if (table.empty()) continue;
            for (const std::string suffix : {"", "_game"}) {
                const std::string file = std::string(language) + suffix + ".loc";
                const auto host = hostPathOf("/Art/fe/" + file);
                std::vector<uint8_t> loc;
                if (!host || !ReadFile(*host, loc) || loc.size() < 20 || std::string(loc.begin(), loc.begin() + 4) != "NLOC") continue;
                const uint32_t count = Be32(loc, 12);
                const size_t tableStart = 20 + static_cast<size_t>(count) * 8;
                if (tableStart > loc.size()) continue;
                std::map<uint32_t, uint32_t> entries;  // key hash -> offset (16-bit units)
                for (uint32_t i = 0; i < count; ++i) entries[Be32(loc, 20 + i * 8)] = Be32(loc, 20 + i * 8 + 4);
                std::vector<uint8_t> text(loc.begin() + static_cast<std::ptrdiff_t>(tableStart), loc.end());
                while (text.size() >= 2 && text[text.size() - 1] == 0 && text[text.size() - 2] == 0) text.resize(text.size() - 2);
                text.push_back(0); text.push_back(0);  // the last string's terminator
                for (const auto& [key, value] : table) {
                    entries[LowerHash(key)] = static_cast<uint32_t>(text.size() / 2);
                    // UTF-8 -> UTF-16BE (the Basic Multilingual Plane, as the game's fonts)
                    for (size_t i = 0; i < value.size();) {
                        uint32_t cp = static_cast<unsigned char>(value[i]);
                        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
                        cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
                        for (++i; extra-- > 0 && i < value.size(); ++i) cp = (cp << 6) | (static_cast<unsigned char>(value[i]) & 0x3F);
                        if (cp > 0xFFFF) cp = '?';
                        text.push_back(static_cast<uint8_t>(cp >> 8)); text.push_back(static_cast<uint8_t>(cp));
                    }
                    text.push_back(0); text.push_back(0);
                    if (std::find(g_locAdded.begin(), g_locAdded.end(), key) == g_locAdded.end()) g_locAdded.push_back(key);
                }
                std::vector<uint8_t> out(loc.begin(), loc.begin() + 20);
                Put32(out, 12, static_cast<uint32_t>(entries.size()));
                out.resize(20 + entries.size() * 8);
                size_t i = 0;
                for (const auto& [hash, offset] : entries) {  // sorted by hash, as the game's binary search needs
                    Put32(out, 20 + i * 8, hash);
                    Put32(out, 20 + i * 8 + 4, offset);
                    ++i;
                }
                out.insert(out.end(), text.begin(), text.end());
                out.resize((out.size() + 31) & ~static_cast<size_t>(31), 0);
                const std::filesystem::path path = locCache / file;
                if (!WriteCache(path, out.data(), out.size())) continue;
                registerFile("/Art/fe/" + file, path, static_cast<uint32_t>(out.size()));
                ++rebuilt;
            }
        }
        RT_LOG(RT_TAG_MODS) << "catalogs: " << g_locAdded.size() << " string(s) added to the localisation tables" << std::endl;
    }
    return rebuilt;
}

// Cutscenes: catalogs/nis/nis_dict.txt appended to Art/nis/nis_dict.txt; triggers.txt / cues.txt aliases.
int MergeCutscenes(const HostPathOf& hostPathOf, const RegisterFile& registerFile) {
    int rebuilt = 0;
    // Cutscenes: dictionary entries, trigger script and cue aliases.
    std::string dict;
    for (const Package* package : ActivePackages()) {
        const std::filesystem::path root = package->root / "catalogs" / "nis";
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        std::vector<uint8_t> bytes;
        if (ReadFile(root / "nis_dict.txt", bytes))
            for (const std::string& line : Lines(std::string(bytes.begin(), bytes.end()))) dict += line + "\r\n";
        for (const char* file : {"triggers.txt", "cues.txt"}) {
            if (!ReadFile(root / file, bytes)) continue;
            for (const auto& [from, to] : Assignments(bytes)) {
                if (to.empty()) continue;
                if (file[0] == 't') {
                    g_nisTriggers[from] = to;
                } else {
                    g_nisCues[LowerHash(from)] = LowerHash(to);
                    g_nisCues[LowerHash(from + "_nocrowd")] = LowerHash(to + "_nocrowd");
                }
            }
        }
    }
    if (!dict.empty()) {
        const auto host = hostPathOf("/Art/nis/nis_dict.txt");
        std::vector<uint8_t> original;
        if (host && ReadFile(*host, original)) {
            std::string merged(original.begin(), original.end());
            while (!merged.empty() && (merged.back() == '\0' || merged.back() == ' ')) merged.pop_back();
            if (!merged.empty() && merged.back() != '\n') merged += "\r\n";
            merged += dict;
            const std::filesystem::path out = CacheDir("nis") / "nis_dict.txt";
            if (WriteCache(out, merged.data(), merged.size())) {
                registerFile("/Art/nis/nis_dict.txt", out, static_cast<uint32_t>(merged.size()));
                RT_LOG(RT_TAG_MODS) << "catalogs: /Art/nis/nis_dict.txt + " << std::count(dict.begin(), dict.end(), '\n')
                                    << " line(s), " << g_nisTriggers.size() << " trigger and " << g_nisCues.size() / 2 << " cue alias(es)" << std::endl;
                ++rebuilt;
            }
        }
    }
    return rebuilt;
}

// Per-character files the game opens by name (crowd lists): a captain without an away-kit list gets
// its home list, and a mod character without its own gets its base's, registered under its name.
int AliasPerCharacterFiles(const HostPathOf& hostPathOf, const RegisterFile& registerFile) {
    int rebuilt = 0;
    const auto alias = [&](const std::string& mine, const std::string& theirs) {
        if (hostPathOf("/" + mine)) return;
        const auto host = hostPathOf("/" + theirs);
        std::error_code ec;
        if (!host) return;
        const auto size = std::filesystem::file_size(*host, ec);
        if (!ec) registerFile("/" + mine, *host, static_cast<uint32_t>(size));
    };
    // The match's crowd comes from the home captain's list, its Alt list when the team wears its away
    // kit. Mario, Luigi, Peach, Wario and Waluigi have none (their teams never switched kit), and a
    // missing list crashes the load: Blue Peach, kit choice and mod characters can put them in one,
    // so they get their home list.
    for (int captain = 0; captain < 12; ++captain) {
        const std::string name = BaseCharacterName(captain);
        alias("ini/CrowdCharacterLists/" + name + "Alt.ini", "ini/CrowdCharacterLists/" + name + ".ini");
    }
    // Per-character files the game opens by the character's name: a mod character without its own
    // gets its base's.
    static const char* const kPerCharacterFiles[] = {"ini/CrowdCharacterLists/%s.ini", "ini/CrowdCharacterLists/%sAlt.ini"};
    for (const Package* package : ActivePackages()) {
        for (const CharacterDef& c : package->characters) {
            const char* base = BaseCharacterName(c.baseIndex);
            if (!base) continue;
            for (const char* pattern : kPerCharacterFiles) {
                char mine[160], theirs[160];
                std::snprintf(mine, sizeof(mine), pattern, c.name.c_str());
                std::snprintf(theirs, sizeof(theirs), pattern, base);
                alias(mine, theirs);
            }
        }
    }
    return rebuilt;
}

// Streamed sounds: catalogs/streams/<bank>/<cue>.idsp into audio/<bank>.resbun + .nlxwb.
int MergeStreams(const HostPathOf& hostPathOf, const RegisterFile& registerFile, const AppendFile& appendFile) {
    int rebuilt = 0;
    // Streamed sounds, per bank: the merged .resbun replaces the bank's, the streams follow its .nlxwb.
    struct BankAdditions {
        std::vector<NewStream> streams;
        std::string templateCue;
        std::string from;
    };
    std::map<std::string, BankAdditions> banks;
    for (const Package* package : ActivePackages()) {
        const std::filesystem::path root = package->root / "catalogs" / "streams";
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& dir : std::filesystem::directory_iterator(root, ec)) {
            if (!dir.is_directory(ec)) continue;
            BankAdditions& add = banks[dir.path().filename().string()];
            std::map<std::string, std::vector<std::string>> aliases;  // stream -> more cue names
            std::vector<uint8_t> text;
            if (ReadFile(dir.path() / "cues.txt", text)) {
                for (const auto& [name, stream] : Assignments(text)) {
                    if (name == "template") add.templateCue = stream;
                    else if (!stream.empty()) aliases[stream].push_back(name);
                }
            }
            std::vector<std::filesystem::path> files;
            for (const auto& f : std::filesystem::directory_iterator(dir.path(), ec))
                if (f.is_regular_file(ec) && f.path().extension() == ".idsp") files.push_back(f.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                NewStream stream;
                stream.names.push_back(f.stem().string());
                for (const std::string& alias : aliases[f.stem().string()]) stream.names.push_back(alias);
                if (ReadFile(f, stream.blob)) add.streams.push_back(std::move(stream));
            }
            add.from += (add.from.empty() ? "" : ", ") + package->id;
        }
    }
    for (auto& [bank, add] : banks) {
        if (add.streams.empty()) continue;
        const std::string resbunPath = "/audio/" + bank + ".resbun", nlxwbPath = "/audio/" + bank + ".nlxwb";
        const auto resbunHost = hostPathOf(resbunPath), nlxwbHost = hostPathOf(nlxwbPath);
        std::vector<uint8_t> original, merged, append;
        std::error_code ec;
        const auto nlxwbSize = nlxwbHost ? std::filesystem::file_size(*nlxwbHost, ec) : 0;
        std::string error = "the bank isn't on the disc";
        if (!resbunHost || !nlxwbHost || ec || nlxwbSize > 0xF0000000u || !ReadFile(*resbunHost, original) ||
            !MergeStreamBank(original, static_cast<uint32_t>(nlxwbSize), add.streams, add.templateCue, merged, append, error)) {
            RT_LOG(RT_TAG_MODS) << "catalogs: audio/" << bank << " (" << add.from << "): " << error << std::endl;
            continue;
        }
        const std::filesystem::path resbunOut = CacheDir("streams") / (bank + ".resbun");
        const std::filesystem::path appendOut = CacheDir("streams") / (bank + ".nlxwb.append");
        if (!WriteCache(resbunOut, merged.data(), merged.size()) || !WriteCache(appendOut, append.data(), append.size())) continue;
        const auto at = appendFile(nlxwbPath, appendOut, static_cast<uint32_t>(append.size()));
        if (!at || *at != nlxwbSize) {
            RT_LOG(RT_TAG_MODS) << "catalogs: audio/" << bank << ".nlxwb: can't append the streams" << std::endl;
            continue;
        }
        registerFile(resbunPath, resbunOut, static_cast<uint32_t>(merged.size()));
        size_t cueCount = 0;
        for (const NewStream& stream : add.streams) cueCount += stream.names.size();
        RT_LOG(RT_TAG_MODS) << "catalogs: audio/" << bank << " + " << add.streams.size() << " stream(s), " << cueCount
                            << " cue(s) from " << add.from << std::endl;
        ++rebuilt;
    }
    return rebuilt;
}

}  // namespace

int Merge(const HostPathOf& hostPathOf, const RegisterFile& registerFile, const AppendFile& appendFile) {
    int rebuilt = MergeFrontEnd(hostPathOf, registerFile);
    rebuilt += MergeInis(hostPathOf, registerFile);
    rebuilt += MergeStrings(hostPathOf, registerFile);
    rebuilt += MergeCutscenes(hostPathOf, registerFile);
    rebuilt += AliasPerCharacterFiles(hostPathOf, registerFile);
    rebuilt += MergeStreams(hostPathOf, registerFile, appendFile);
    return rebuilt;
}

std::filesystem::path FeTextureFile(const std::string& name) {
    const auto it = g_feFiles.find(LowerHash(name));
    return it == g_feFiles.end() ? std::filesystem::path{} : it->second;
}

std::string NisTriggerAlias(const std::string& nis) {
    const auto it = g_nisTriggers.find(nis);
    return it == g_nisTriggers.end() ? std::string() : it->second;
}

uint32_t NisCueAlias(uint32_t cueHash) {
    const auto it = g_nisCues.find(cueHash);
    return it == g_nisCues.end() ? 0 : it->second;
}

bool HasLocKey(const std::string& key) {
    return std::find(g_locAdded.begin(), g_locAdded.end(), key) != g_locAdded.end();
}

bool HasFeTexture(const std::string& name) {
    return std::find(g_feAdded.begin(), g_feAdded.end(), LowerHash(name)) != g_feAdded.end();
}

}  // namespace Mods::Catalogs
