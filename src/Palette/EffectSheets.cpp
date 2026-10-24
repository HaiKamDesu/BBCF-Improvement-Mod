#include "EffectSheets.h"

#include "NativePalettes.h"
#include "PaletteThumbnails.h"

#include "Core/EmbeddedResources.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include "miniz.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace
{
	// Blob layout, written by tools/build_effect_palette_map.py:
	//   "BBEM", u32 version, u32 count, count * { u32 offset, u32 entries },
	//   per entry: u8 pack (0 img, 1 vri), u8 file mask (bit n = file n), u8 length, name,
	//   u8 blend mode, u8 palette recipe, and when the recipe is not 0: 8 * i32 for 3041-3048;
	//   after a character's entries: u8 files, files * { u8 file, u16 count, count * u8 entry }
	const wchar_t* const kMapResource = L"effect_palette_map";
	const unsigned int kMapVersion = 3;

	// Every sprite is shrunk to fit this box on the sheet (never enlarged); if a file has
	// so many that the sheet would outgrow a texture, the box shrinks until it fits.
	const int kSheetWidth = 1400;
	const int kMaxSheetHeight = 4096;
	const int kSpriteBoxes[] = { 200, 160, 128, 96, 72 };
	const int kPadding = 10;

	unsigned int ReadU32(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
	}

	// How the game draws an effect object (BBCF.exe; see tools/build_effect_palette_map.py).
	enum Blend
	{
		Blend_Opaque = 0,        // 3031, default: drawn as is, alpha-tested
		Blend_Alpha = 1,         // 3032
		Blend_Additive = 2,      // 3033: black adds nothing, which is why these have black boxes
		Blend_Premultiplied = 3, // 3034
		Blend_Subtractive = 4,   // 3035
	};

	struct RenderInfo
	{
		unsigned char blend = Blend_Opaque;
		// 3040: 0 none, 1 gradient dissolve, 2 three-colour fade. With a recipe the drawn
		// colours are rebuilt from a few palette entries over the sprite's index, and the
		// entries under the sprite itself do not show at all.
		unsigned char recipe = 0;
		int params[8] = {};      // 3041..3048
	};

	struct MapEntry
	{
		int pack;
		unsigned char mask;
		std::string name;
		RenderInfo render;
	};

	// Per character and file (charIndex * 8 + file): the entries its scripts take single
	// colours from - 3D models and particles tinted with one palette colour, drawn on no
	// sprite (setParticleColorFromPalette and friends). Filled by Map().
	std::vector<std::vector<unsigned char>> g_coloursRead;

	const std::vector<std::vector<MapEntry>>& Map()
	{
		static std::vector<std::vector<MapEntry>> map;
		static bool loaded = false;
		if (loaded)
			return map;
		loaded = true;

		std::string blob;
		if (!LoadEmbeddedResource(kMapResource, blob) || blob.size() < 12)
			return map;
		const unsigned char* base = (const unsigned char*)blob.data();
		if (memcmp(base, "BBEM", 4) != 0 || ReadU32(base + 4) != kMapVersion)
			return map;

		const unsigned int count = ReadU32(base + 8);
		if (blob.size() < 12 + (size_t)count * 8)
			return map;
		map.resize(count);
		g_coloursRead.resize((size_t)count * 8);
		for (unsigned int c = 0; c < count; c++)
		{
			size_t pos = ReadU32(base + 12 + c * 8);
			const unsigned int entries = ReadU32(base + 12 + c * 8 + 4);
			unsigned int e = 0;
			for (; e < entries && pos + 3 <= blob.size(); e++)
			{
				MapEntry entry;
				entry.pack = base[pos];
				entry.mask = base[pos + 1];
				const size_t length = base[pos + 2];
				pos += 3;
				if (pos + length > blob.size())
					break;
				entry.name.assign((const char*)base + pos, length);
				pos += length;
				if (pos + 2 > blob.size())
					break;
				entry.render.blend = base[pos];
				entry.render.recipe = base[pos + 1];
				pos += 2;
				if (entry.render.recipe)
				{
					if (pos + 32 > blob.size())
						break;
					for (int k = 0; k < 8; k++)
						entry.render.params[k] = (int)ReadU32(base + pos + k * 4);
					pos += 32;
				}
				map[c].push_back(entry);
			}
			if (e < entries || pos >= blob.size())
				continue; // truncated: no colour list to read
			const unsigned int files = base[pos++];
			for (unsigned int f = 0; f < files && pos + 3 <= blob.size(); f++)
			{
				const unsigned int file = base[pos];
				const size_t length = base[pos + 1] | (base[pos + 2] << 8);
				pos += 3;
				if (pos + length > blob.size())
					break;
				if (file >= 1 && file <= 7)
					g_coloursRead[(size_t)c * 8 + file].assign(base + pos, base + pos + length);
				pos += length;
			}
		}
		return map;
	}

	// One effect image, cropped and shrunk, ready to lay out.
	struct Sprite
	{
		std::string name;
		unsigned char mask = 0;
		RenderInfo render;
		int width = 0;
		int height = 0;
		std::vector<unsigned char> pixels;
	};

	// Indexed HIP -> cropped palette indices. HIPs without a palette are true-colour and
	// are not drawn with a palette file at all, so they are skipped.
	bool DecodeHip(const std::vector<unsigned char>& blob, Sprite& out)
	{
		if (blob.size() < 0x20 || memcmp(blob.data(), "HIP", 3) != 0)
			return false;
		const unsigned char* p = blob.data();
		const unsigned int paletteCount = ReadU32(p + 0x0C);
		unsigned int width = ReadU32(p + 0x10);
		unsigned int height = ReadU32(p + 0x14);
		const unsigned int extra = ReadU32(p + 0x1C);
		size_t offset = 0x20;
		if (extra >= 0x10 && blob.size() >= 0x28)
		{
			width = ReadU32(p + 0x20);
			height = ReadU32(p + 0x24);
			offset = 0x20 + extra;
		}
		if (paletteCount == 0 || width == 0 || height == 0 || width > 4096 || height > 4096)
			return false;
		offset += (size_t)paletteCount * 4;

		std::vector<unsigned char> pixels((size_t)width * height, 0);
		size_t written = 0;
		// Zero-length runs do occur in shipped sprites, so this bounds on input as well.
		while (written < pixels.size() && offset + 1 < blob.size())
		{
			const unsigned char index = blob[offset];
			const size_t run = (std::min)((size_t)blob[offset + 1], pixels.size() - written);
			offset += 2;
			if (index)
				memset(&pixels[written], index, run);
			written += run;
		}

		// Crop to what is drawn.
		int left = (int)width, top = (int)height, right = -1, bottom = -1;
		for (unsigned int y = 0; y < height; y++)
		{
			const unsigned char* row = &pixels[(size_t)y * width];
			for (unsigned int x = 0; x < width; x++)
			{
				if (!row[x])
					continue;
				left = (std::min)(left, (int)x);
				right = (std::max)(right, (int)x);
				top = (std::min)(top, (int)y);
				bottom = (std::max)(bottom, (int)y);
			}
		}
		if (right < 0)
			return false; // empty

		out.width = right - left + 1;
		out.height = bottom - top + 1;
		out.pixels.resize((size_t)out.width * out.height);
		for (int y = 0; y < out.height; y++)
			memcpy(&out.pixels[(size_t)y * out.width], &pixels[(size_t)(top + y) * width + left], out.width);
		return true;
	}

	// Shrinks a sprite to fit `box`, in index space: a sample that lands on transparency
	// takes any drawn pixel from its block instead, so thin strokes survive.
	void FitSprite(const Sprite& in, int box, int& outW, int& outH, std::vector<unsigned char>& out)
	{
		const float scale = (std::min)(1.0f, (std::min)((float)box / in.width, (float)box / in.height));
		outW = (std::max)(1, (int)(in.width * scale));
		outH = (std::max)(1, (int)(in.height * scale));
		out.assign((size_t)outW * outH, 0);
		for (int y = 0; y < outH; y++)
		{
			const int sy0 = (int)(y / scale);
			const int sy1 = (std::min)(in.height, (std::max)(sy0 + 1, (int)((y + 1) / scale)));
			for (int x = 0; x < outW; x++)
			{
				const int sx0 = (int)(x / scale);
				const int sx1 = (std::min)(in.width, (std::max)(sx0 + 1, (int)((x + 1) / scale)));
				unsigned char value = in.pixels[(size_t)((sy0 + sy1) / 2) * in.width + (sx0 + sx1) / 2];
				for (int sy = sy0; !value && sy < sy1; sy++)
					for (int sx = sx0; !value && sx < sx1; sx++)
						value = in.pixels[(size_t)sy * in.width + sx];
				out[(size_t)y * outW + x] = value;
			}
		}
	}

	// Streams a .pac through the inflater and hands over only the entries asked for.
	// The DFASFPAC envelope is one zlib stream over the whole FPAC; inflating it into a
	// 32 KB window and copying out just the wanted byte ranges is what keeps a 30 MB
	// character archive from ever being resident.
	class PacStreamer
	{
	public:
		PacStreamer(const std::set<std::string>& wanted) : m_wanted(wanted) {}

		void Feed(const unsigned char* data, size_t size)
		{
			while (size > 0 && !m_failed)
			{
				if (!m_tableParsed)
				{
					const size_t need = m_dataStart ? m_dataStart : 0x20;
					const size_t take = (std::min)(size, need > m_head.size() ? need - m_head.size() : 0);
					m_head.insert(m_head.end(), data, data + take);
					m_position += take;
					data += take;
					size -= take;
					if (m_head.size() < need)
						return;
					if (!m_dataStart)
					{
						if (memcmp(m_head.data(), "FPAC", 4) != 0)
						{
							m_failed = true;
							return;
						}
						m_dataStart = ReadU32(&m_head[4]);
						if (m_dataStart < 0x20 || m_dataStart > 64u * 1024u * 1024u)
							m_failed = true;
						continue;
					}
					ParseTable();
					continue;
				}

				// Skip ahead to the next wanted entry, or copy into it.
				if (m_next >= m_entries.size())
					return;
				Entry& entry = m_entries[m_next];
				if (m_position < entry.start)
				{
					const size_t skip = (size_t)(std::min)((unsigned long long)size, entry.start - m_position);
					m_position += skip;
					data += skip;
					size -= skip;
					continue;
				}
				const size_t take = (size_t)(std::min)((unsigned long long)size, entry.end - m_position);
				entry.bytes.insert(entry.bytes.end(), data, data + take);
				m_position += take;
				data += take;
				size -= take;
				if (m_position >= entry.end)
				{
					m_done.push_back(std::make_pair(entry.name, std::move(entry.bytes)));
					m_next++;
					// Entries can overlap in principle; start any that began inside this one.
					while (m_next < m_entries.size() && m_entries[m_next].start < m_position)
						m_next++;
				}
			}
		}

		bool Failed() const { return m_failed; }
		bool Finished() const { return m_tableParsed && m_next >= m_entries.size(); }

		// Entries completed since the last call.
		std::vector<std::pair<std::string, std::vector<unsigned char>>> TakeDone()
		{
			std::vector<std::pair<std::string, std::vector<unsigned char>>> done;
			done.swap(m_done);
			return done;
		}

	private:
		struct Entry
		{
			std::string name;
			unsigned long long start;
			unsigned long long end;
			std::vector<unsigned char> bytes;
		};

		void ParseTable()
		{
			m_tableParsed = true;
			const unsigned int count = ReadU32(&m_head[0x0C]);
			const unsigned int nameField = ReadU32(&m_head[0x14]);
			if (count == 0 || nameField < 4 || nameField > 256)
			{
				m_failed = true;
				return;
			}
			const unsigned int stride = (m_dataStart - 0x20) / count;
			if (stride < nameField + 12)
			{
				m_failed = true;
				return;
			}
			for (unsigned int i = 0; i < count; i++)
			{
				const size_t record = 0x20 + (size_t)i * stride;
				if (record + nameField + 12 > m_head.size())
					break;
				const char* name = (const char*)&m_head[record];
				size_t length = 0;
				while (length < nameField && name[length])
					length++;
				std::string entryName(name, length);
				const size_t dot = entryName.rfind('.');
				if (dot != std::string::npos)
					entryName.resize(dot);
				if (!m_wanted.count(entryName))
					continue;
				Entry entry;
				entry.name = entryName;
				entry.start = (unsigned long long)m_dataStart + ReadU32(&m_head[record + nameField + 4]);
				entry.end = entry.start + ReadU32(&m_head[record + nameField + 8]);
				m_entries.push_back(entry);
			}
			std::sort(m_entries.begin(), m_entries.end(),
				[](const Entry& a, const Entry& b) { return a.start < b.start; });
			std::vector<unsigned char>().swap(m_head);
		}

		const std::set<std::string>& m_wanted;
		std::vector<unsigned char> m_head;
		unsigned int m_dataStart = 0;
		bool m_tableParsed = false;
		bool m_failed = false;
		unsigned long long m_position = 0;
		std::vector<Entry> m_entries;
		size_t m_next = 0;
		std::vector<std::pair<std::string, std::vector<unsigned char>>> m_done;
	};

	bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out)
	{
		std::ifstream file(utf8_to_utf16(path).c_str(), std::ios::binary | std::ios::ate);
		if (!file)
			return false;
		const std::streamoff size = file.tellg();
		if (size <= 0 || size > 256ll * 1024 * 1024)
			return false;
		out.resize((size_t)size);
		file.seekg(0);
		file.read((char*)out.data(), size);
		return (bool)file;
	}

	// Pulls the wanted images out of one archive, decoding each as it completes.
	bool ExtractSprites(const std::string& path, const std::map<std::string, const MapEntry*>& wanted,
		std::vector<Sprite>& out, std::string& error)
	{
		if (wanted.empty())
			return true;

		std::vector<unsigned char> file;
		if (!ReadWholeFile(path, file))
		{
			error = "Could not read " + path;
			return false;
		}

		std::set<std::string> names;
		for (const auto& w : wanted)
			names.insert(w.first);
		PacStreamer streamer(names);

		auto collect = [&]() {
			for (auto& done : streamer.TakeDone())
			{
				Sprite sprite;
				if (DecodeHip(done.second, sprite))
				{
					sprite.name = done.first;
					const MapEntry* entry = wanted.at(done.first);
					sprite.mask = entry->mask;
					sprite.render = entry->render;
					out.push_back(std::move(sprite));
				}
			}
		};

		if (file.size() >= 0x10 && memcmp(file.data(), "DFASFPAC", 8) == 0)
		{
			const size_t compressed = (std::min)((size_t)ReadU32(&file[0x0C]), file.size() - 0x10);
			std::unique_ptr<tinfl_decompressor> inflater(new tinfl_decompressor);
			tinfl_init(inflater.get());
			std::vector<unsigned char> window(TINFL_LZ_DICT_SIZE);
			size_t inPos = 0;
			size_t windowPos = 0;
			for (;;)
			{
				size_t inBytes = compressed - inPos;
				size_t outBytes = TINFL_LZ_DICT_SIZE - windowPos;
				const tinfl_status status = tinfl_decompress(inflater.get(), &file[0x10 + inPos], &inBytes,
					window.data(), window.data() + windowPos, &outBytes, TINFL_FLAG_PARSE_ZLIB_HEADER);
				inPos += inBytes;
				streamer.Feed(window.data() + windowPos, outBytes);
				collect();
				windowPos = (windowPos + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
				if (streamer.Failed())
				{
					error = path + " is not in a format this build understands.";
					return false;
				}
				if (streamer.Finished() || status == TINFL_STATUS_DONE)
					break;
				if (status < TINFL_STATUS_DONE || (status == TINFL_STATUS_NEEDS_MORE_INPUT && inPos >= compressed))
				{
					error = path + " is damaged.";
					return false;
				}
			}
		}
		else
		{
			streamer.Feed(file.data(), file.size());
			collect();
			if (streamer.Failed())
			{
				error = path + " is not in a format this build understands.";
				return false;
			}
		}
		return true;
	}

	struct Loaded
	{
		int charIndex = -1;
		std::vector<Sprite> sprites;
	};


	std::mutex g_mutex;
	int g_generation = 0;
	int g_requestedChar = -1;
	EffectSheets::Status g_status = EffectSheets::Status_Idle;
	std::string g_error;
	std::shared_ptr<Loaded> g_loaded;

	// The composed sheet: UI thread only.
	int g_sheetChar = -1;
	int g_sheetFile = -1;
	int g_sheetWidth = 0;
	int g_sheetHeight = 0;
	std::vector<unsigned char> g_sheet;          // the sprites' own palette indices
	std::vector<unsigned short> g_sheetSprite;   // per pixel: 1 + which sprite drew it, 0 for none
	std::vector<unsigned char> g_sheetPick;      // per pixel: the palette entry that decides its colour
	std::vector<RenderInfo> g_sheetRender;       // per placed sprite

	// The last Render(), kept so an unchanged frame costs a few compares.
	std::vector<unsigned int> g_rendered;
	unsigned char g_renderedPalette[1024];
	unsigned char g_renderedMask[256];
	bool g_renderedHasMask = false;
	PaletteThumbnails::SheetFade g_renderedFade;
	unsigned int g_renderedBackground = 0;
	bool g_renderedValid = false;

	float Clamp01(float v)
	{
		return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}

	// 3040 mode 2 alpha at index i, frame 0 (BBCF.exe 0x5A6F65).
	float RecipeTwoAlpha(const RenderInfo& r, int i)
	{
		const float e = 0.1f * (r.params[4] - 1790) + i * (1.0f + 0.1f * r.params[7]);
		return Clamp01(1.0f - 0.05f * (1.0f + 0.01f * (r.params[6] - 75)) * e);
	}

	// The entry a click on this pixel should select. Without a recipe that is the pixel's
	// own index; with one, the sprite's own entries are never shown, so it is whichever of
	// the recipe's entries contributes most there.
	unsigned char DominantEntry(const RenderInfo& r, unsigned char index)
	{
		if (!index || !r.recipe)
			return index;
		if (r.recipe == 1)
			return (unsigned char)(r.params[1] & 255); // weight of the first colour never drops below half
		if (RecipeTwoAlpha(r, index) < 0.5f)
			return (unsigned char)(r.params[0] & 255);
		return (unsigned char)((index < 128 ? r.params[1] : r.params[2]) & 255);
	}

	// Is any entry that makes this pixel's colour in the highlight?
	bool Highlighted(const RenderInfo& r, unsigned char index, const unsigned char* mask)
	{
		if (!r.recipe)
			return mask[index] != 0;
		if (mask[r.params[1] & 255] || mask[r.params[2] & 255])
			return true;
		return r.recipe == 2 && mask[r.params[0] & 255];
	}

	struct Rgba
	{
		float r, g, b, a; // 0..255
	};

	Rgba EntryRgba(const unsigned char* palette, int index)
	{
		const unsigned char* e = palette + (index & 255) * 4;
		Rgba c = { (float)e[2], (float)e[1], (float)e[0], (float)e[3] };
		return c;
	}

	// The colours a sprite is drawn with, per index, as BBCF.exe builds them (0x5A6C80;
	// frame 0 of any timed recipe, which is how an effect first appears).
	void BuildLut(const RenderInfo& r, const unsigned char* palette, Rgba* lut)
	{
		for (int i = 0; i < 256; i++)
		{
			const float x = i / 255.0f;
			if (r.recipe == 1)
			{
				const Rgba a = EntryRgba(palette, r.params[1]);
				const Rgba b = EntryRgba(palette, r.params[2]);
				const float w = 1.0f - x * 0.5f;
				lut[i].r = (float)(int)(a.r * w + b.r * (1.0f - w));
				lut[i].g = (float)(int)(a.g * w + b.g * (1.0f - w));
				lut[i].b = (float)(int)(a.b * w + b.b * (1.0f - w));
				lut[i].a = 255.0f;
			}
			else if (r.recipe == 2)
			{
				const Rgba c1 = EntryRgba(palette, r.params[1]);
				const Rgba c2 = EntryRgba(palette, r.params[2]);
				const Rgba c3 = EntryRgba(palette, r.params[0]);
				const float alpha = RecipeTwoAlpha(r, i);
				lut[i].r = (float)(int)((c2.r * x + c1.r * (1.0f - x)) * alpha + c3.r * (1.0f - alpha));
				lut[i].g = (float)(int)((c2.g * x + c1.g * (1.0f - x)) * alpha + c3.g * (1.0f - alpha));
				lut[i].b = (float)(int)((c2.b * x + c1.b * (1.0f - x)) * alpha + c3.b * (1.0f - alpha));
				lut[i].a = (float)(int)(255.0f * alpha);
			}
			else
			{
				lut[i] = EntryRgba(palette, i);
			}
		}
	}

	// Same fade the character sheet uses (PaletteThumbnails), on a finished colour.
	void Fade(Rgba& c, const PaletteThumbnails::SheetFade& fade)
	{
		const float luma = (c.b * 0.114f + c.g * 0.587f + c.r * 0.299f) / 128.0f;
		const float scale = (1.0f - fade.shading) + fade.shading * luma;
		c.r += (fade.red * scale - c.r) * fade.strength;
		c.g += (fade.green * scale - c.g) * fade.strength;
		c.b += (fade.blue * scale - c.b) * fade.strength;
		c.a *= 1.0f + (fade.opacity - 1.0f) * fade.strength;
	}

	unsigned int Composite(const Rgba& c, unsigned char blend, float bgR, float bgG, float bgB)
	{
		float f = Clamp01(c.a / 255.0f);
		float r, g, b;
		switch (blend)
		{
		case Blend_Additive:
			r = bgR + c.r * f; g = bgG + c.g * f; b = bgB + c.b * f;
			break;
		case Blend_Subtractive:
			r = bgR - c.r * f; g = bgG - c.g * f; b = bgB - c.b * f;
			break;
		case Blend_Premultiplied:
			r = c.r + bgR * (1.0f - f); g = c.g + bgG * (1.0f - f); b = c.b + bgB * (1.0f - f);
			break;
		case Blend_Opaque:
			// Alpha-tested: drawn whole wherever it is drawn at all.
			f = c.a > 0.0f ? 1.0f : 0.0f;
			// fall through
		default:
			r = c.r * f + bgR * (1.0f - f); g = c.g * f + bgG * (1.0f - f); b = c.b * f + bgB * (1.0f - f);
			break;
		}
		const unsigned int ir = (unsigned int)(r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r));
		const unsigned int ig = (unsigned int)(g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g));
		const unsigned int ib = (unsigned int)(b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b));
		return 0xFF000000u | (ir << 16) | (ig << 8) | ib;
	}

	void LoadCharacter(int charIndex, int generation)
	{
		std::shared_ptr<Loaded> result(new Loaded);
		result->charIndex = charIndex;
		std::string error;
		bool ok = true;

		const auto& map = Map();
		const char* tag = NativePalettes::CharTag(charIndex);
		if (!tag || charIndex >= (int)map.size())
		{
			ok = false;
			error = "This build has no effect data for this character.";
		}
		else
		{
			std::map<std::string, const MapEntry*> wanted[2];
			for (const MapEntry& entry : map[charIndex])
				if (entry.pack == 0 || entry.pack == 1)
					wanted[entry.pack][entry.name] = &entry;

			const char* packs[2] = { "img", "vri" };
			for (int p = 0; p < 2 && ok; p++)
			{
				const std::string path = GamePath(std::string("data\\Char\\char_") + tag + "_" + packs[p] + ".pac");
				ok = ExtractSprites(path, wanted[p], result->sprites, error);
			}
			std::sort(result->sprites.begin(), result->sprites.end(),
				[](const Sprite& a, const Sprite& b) { return a.name < b.name; });
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		if (generation != g_generation)
			return; // superseded while loading
		if (ok)
		{
			g_loaded = result;
			g_status = EffectSheets::Status_Ready;
			LOG(2, "EffectSheets: %d effect images for character %d\n", (int)result->sprites.size(), charIndex);
		}
		else
		{
			g_status = EffectSheets::Status_Failed;
			g_error = error;
			LOG(2, "EffectSheets: %s\n", error.c_str());
		}
	}

	// Rows of sprites, left to right, each row as tall as its tallest.
	bool Compose(const Loaded& loaded, int file, int box)
	{
		const unsigned char bit = (unsigned char)(1 << file);
		struct Placed
		{
			const Sprite* sprite;
			int w, h, x, y;
			std::vector<unsigned char> pixels;
		};
		std::vector<Placed> placed;
		int x = kPadding, y = kPadding, rowHeight = 0, usedWidth = 0;
		for (const Sprite& sprite : loaded.sprites)
		{
			if (!(sprite.mask & bit))
				continue;
			Placed item;
			item.sprite = &sprite;
			FitSprite(sprite, box, item.w, item.h, item.pixels);
			if (x + item.w + kPadding > kSheetWidth && x > kPadding)
			{
				x = kPadding;
				y += rowHeight + kPadding;
				rowHeight = 0;
			}
			item.x = x;
			item.y = y;
			x += item.w + kPadding;
			usedWidth = (std::max)(usedWidth, x);
			rowHeight = (std::max)(rowHeight, item.h);
			placed.push_back(std::move(item));
		}
		const int height = y + rowHeight + kPadding;
		if (placed.empty() || height > kMaxSheetHeight)
			return false;

		g_sheetWidth = (std::max)(usedWidth, kPadding * 2);
		g_sheetHeight = height;
		const size_t area = (size_t)g_sheetWidth * g_sheetHeight;
		g_sheet.assign(area, 0);
		g_sheetSprite.assign(area, 0);
		g_sheetPick.assign(area, 0);
		g_sheetRender.clear();
		for (const Placed& item : placed)
		{
			g_sheetRender.push_back(item.sprite->render);
			const unsigned short id = (unsigned short)g_sheetRender.size();
			for (int row = 0; row < item.h; row++)
			{
				const size_t at = (size_t)(item.y + row) * g_sheetWidth + item.x;
				const unsigned char* src = &item.pixels[(size_t)row * item.w];
				memcpy(&g_sheet[at], src, item.w);
				for (int col = 0; col < item.w; col++)
				{
					if (!src[col])
						continue;
					g_sheetSprite[at + col] = id;
					g_sheetPick[at + col] = DominantEntry(item.sprite->render, src[col]);
				}
			}
		}
		g_renderedValid = false;
		return true;
	}
}

namespace EffectSheets
{
	int ImageCount(int charIndex, int file)
	{
		const auto& map = Map();
		if (charIndex < 0 || charIndex >= (int)map.size() || file < 1 || file > 7)
			return 0;
		int count = 0;
		for (const MapEntry& entry : map[charIndex])
			if (entry.mask & (1 << file))
				count++;
		return count;
	}

	const std::vector<unsigned char>& ColoursRead(int charIndex, int file)
	{
		static const std::vector<unsigned char> none;
		Map();
		if (charIndex < 0 || file < 1 || file > 7 || (size_t)charIndex * 8 + file >= g_coloursRead.size())
			return none;
		return g_coloursRead[(size_t)charIndex * 8 + file];
	}

	bool FileIsUsed(int charIndex, int file)
	{
		return ImageCount(charIndex, file) > 0 || !ColoursRead(charIndex, file).empty();
	}

	Status Request(int charIndex, std::string* error)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (charIndex != g_requestedChar)
		{
			g_generation++;
			g_requestedChar = charIndex;
			g_loaded.reset();
			g_error.clear();
			g_status = Status_Loading;
			g_sheetChar = -1;
			std::vector<unsigned char>().swap(g_sheet);
			const int generation = g_generation;
			std::thread(LoadCharacter, charIndex, generation).detach();
		}
		if (error)
			*error = g_error;
		return g_status;
	}

	bool GetSheet(int charIndex, int file, const unsigned char** indices, int* width, int* height)
	{
		std::shared_ptr<Loaded> loaded;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			loaded = g_loaded;
		}
		if (!loaded || loaded->charIndex != charIndex)
			return false;

		if (g_sheetChar != charIndex || g_sheetFile != file)
		{
			g_sheetChar = -1;
			bool composed = false;
			for (int box : kSpriteBoxes)
			{
				if (Compose(*loaded, file, box))
				{
					composed = true;
					break;
				}
			}
			if (!composed)
				return false;
			g_sheetChar = charIndex;
			g_sheetFile = file;
		}

		*indices = g_sheetPick.data();
		*width = g_sheetWidth;
		*height = g_sheetHeight;
		return true;
	}

	bool GetRawSheet(int charIndex, int file, const unsigned char** indices, int* width, int* height)
	{
		const unsigned char* pick = nullptr;
		if (!GetSheet(charIndex, file, &pick, width, height))
			return false;
		*indices = g_sheet.data();
		return true;
	}

	const unsigned int* Render(const char* paletteData, const unsigned char* highlightMask,
		const PaletteThumbnails::SheetFade& fade, unsigned int background, bool* changed)
	{
		if (changed)
			*changed = false;
		if (g_sheetChar < 0 || g_sheet.empty() || !paletteData)
			return nullptr;

		const size_t area = (size_t)g_sheetWidth * g_sheetHeight;
		if (g_renderedValid && g_rendered.size() == area && g_renderedBackground == background &&
			memcmp(g_renderedPalette, paletteData, sizeof(g_renderedPalette)) == 0 &&
			g_renderedHasMask == (highlightMask != nullptr) &&
			(!highlightMask || (memcmp(g_renderedMask, highlightMask, 256) == 0 && g_renderedFade == fade)))
		{
			return g_rendered.data();
		}

		// One colour table per sprite: a recipe makes the same index mean a different
		// colour from one sprite to the next.
		const unsigned char* palette = (const unsigned char*)paletteData;
		std::vector<Rgba> luts(g_sheetRender.size() * 256);
		for (size_t i = 0; i < g_sheetRender.size(); i++)
			BuildLut(g_sheetRender[i], palette, &luts[i * 256]);

		const float bgR = (float)((background >> 16) & 255);
		const float bgG = (float)((background >> 8) & 255);
		const float bgB = (float)(background & 255);
		const unsigned int bgPixel = 0xFF000000u | (background & 0xFFFFFFu);

		g_rendered.resize(area);
		for (size_t p = 0; p < area; p++)
		{
			const unsigned short id = g_sheetSprite[p];
			if (!id)
			{
				g_rendered[p] = bgPixel;
				continue;
			}
			const RenderInfo& render = g_sheetRender[id - 1];
			const unsigned char index = g_sheet[p];
			Rgba colour = luts[(size_t)(id - 1) * 256 + index];
			if (highlightMask && !Highlighted(render, index, highlightMask))
				Fade(colour, fade);
			g_rendered[p] = Composite(colour, render.blend, bgR, bgG, bgB);
		}

		memcpy(g_renderedPalette, paletteData, sizeof(g_renderedPalette));
		g_renderedHasMask = highlightMask != nullptr;
		if (highlightMask)
			memcpy(g_renderedMask, highlightMask, 256);
		g_renderedFade = fade;
		g_renderedBackground = background;
		g_renderedValid = true;
		if (changed)
			*changed = true;
		return g_rendered.data();
	}

	void Release()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_requestedChar < 0)
			return;
		g_generation++;
		g_requestedChar = -1;
		g_loaded.reset();
		g_status = Status_Idle;
		g_error.clear();
		g_sheetChar = -1;
		std::vector<unsigned char>().swap(g_sheet);
		std::vector<unsigned short>().swap(g_sheetSprite);
		std::vector<unsigned char>().swap(g_sheetPick);
		std::vector<unsigned int>().swap(g_rendered);
		g_renderedValid = false;
	}
}
