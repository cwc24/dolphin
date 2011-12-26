
#include "MemoryUtil.h"

#include "VideoConfig.h"
#include "Statistics.h"
#include "HiresTextures.h"
#include "RenderBase.h"
#include "FileUtil.h"

#include "TextureCacheBase.h"
#include "Debugger.h"
#include "ConfigManager.h"
#include "HW/Memmap.h"

// ugly
extern int frameCount;

enum
{
	TEMP_SIZE = (2048 * 2048 * 4),
	TEXTURE_KILL_THRESHOLD = 200,
};

TextureCache *g_texture_cache;

GC_ALIGNED16(u8 *TextureCache::temp) = NULL;

TextureCache::TexCache TextureCache::textures;
bool TextureCache::DeferredInvalidate;

TextureCache::TCacheEntryBase::~TCacheEntryBase()
{
}

TextureCache::TextureCache()
{
	if (!temp)
		temp = (u8*)AllocateAlignedMemory(TEMP_SIZE,16);
	TexDecoder_SetTexFmtOverlayOptions(g_ActiveConfig.bTexFmtOverlayEnable, g_ActiveConfig.bTexFmtOverlayCenter);
    if(g_ActiveConfig.bHiresTextures && !g_ActiveConfig.bDumpTextures)
		HiresTextures::Init(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str());
	SetHash64Function(g_ActiveConfig.bHiresTextures || g_ActiveConfig.bDumpTextures);
}

void TextureCache::Invalidate(bool shutdown)
{
	TexCache::iterator
		iter = textures.begin(),
		tcend = textures.end();
	for (; iter != tcend; ++iter)
	{
		if (shutdown)
			iter->second->addr = 0;
		delete iter->second;
	}

	textures.clear();
	if(g_ActiveConfig.bHiresTextures && !g_ActiveConfig.bDumpTextures)
		HiresTextures::Init(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str());
	SetHash64Function(g_ActiveConfig.bHiresTextures || g_ActiveConfig.bDumpTextures);
	
	DeferredInvalidate = false;
}

void TextureCache::InvalidateDefer()
{
	DeferredInvalidate = true;
}

TextureCache::~TextureCache()
{
	Invalidate(true);
	if (temp)
	{
		FreeAlignedMemory(temp);
		temp = NULL;
	}
}

void TextureCache::Cleanup()
{
	TexCache::iterator iter = textures.begin();
	TexCache::iterator tcend = textures.end();
	while (iter != tcend)
	{
		if (frameCount > TEXTURE_KILL_THRESHOLD + iter->second->frameCount) // TODO: Deleting EFB copies might not be a good idea here...
		{
			delete iter->second;
			textures.erase(iter++);
		}
		else
			++iter;
	}
}

void TextureCache::InvalidateRange(u32 start_address, u32 size)
{
	TexCache::iterator
		iter = textures.begin(),
		tcend = textures.end();
	while (iter != tcend)
	{
		const int rangePosition = iter->second->IntersectsMemoryRange(start_address, size);
		if (0 == rangePosition)
		{
			delete iter->second;
			textures.erase(iter++);
		}
		else
			++iter;
	}
}

void TextureCache::MakeRangeDynamic(u32 start_address, u32 size)
{
	TexCache::iterator
		iter = textures.lower_bound(start_address),
		tcend = textures.upper_bound(start_address + size);

	if (iter != textures.begin())
		iter--;

	for (; iter != tcend; ++iter)
	{
		const int rangePosition = iter->second->IntersectsMemoryRange(start_address, size);
		if (0 == rangePosition)
		{
			iter->second->SetHashes(TEXHASH_INVALID);
		}
	}
}

bool TextureCache::Find(u32 start_address, u64 hash)
{
	TexCache::iterator iter = textures.lower_bound(start_address);

	if (iter->second->hash == hash)
		return true;

	return false;
}

int TextureCache::TCacheEntryBase::IntersectsMemoryRange(u32 range_address, u32 range_size) const
{
	if (addr + size_in_bytes < range_address)
		return -1;

	if (addr >= range_address + range_size)
		return 1;

	return 0;
}

void TextureCache::ClearRenderTargets()
{
	TexCache::iterator
		iter = textures.begin(),
		tcend = textures.end();
	for (; iter!=tcend; ++iter)
		iter->second->efbcopy_state = EC_NO_COPY;
}

TextureCache::TCacheEntryBase* TextureCache::Load(unsigned int stage,
	u32 address, unsigned int width, unsigned int height, int texformat,
	unsigned int tlutaddr, int tlutfmt, bool UseNativeMips, unsigned int maxlevel)
{
	if (0 == address)
		return NULL;

	// TexelSizeInNibbles(format)*width*height/16;
	const unsigned int bsw = TexDecoder_GetBlockWidthInTexels(texformat) - 1;
	const unsigned int bsh = TexDecoder_GetBlockHeightInTexels(texformat) - 1;

	unsigned int expandedWidth  = (width  + bsw) & (~bsw);
	unsigned int expandedHeight = (height + bsh) & (~bsh);
	const unsigned int nativeW = width;
	const unsigned int nativeH = height;

	// TODO: Force STC enabled when using custom textures or when dumping textures. There's no need for having two different texture hashes then.
	u32 texID = address;
	u64 hash_value = TEXHASH_INVALID; // Hash assigned to texcache entry
	u64 texHash = TEXHASH_INVALID; // Accurate hash used for texture dumping, hires texture lookup. Equal to hash_value with STC.
	u64 tlut_hash = TEXHASH_INVALID;

	u32 full_format = texformat;
	PC_TexFormat pcfmt = PC_TEX_FMT_NONE;

	const bool isPaletteTexture = (texformat == GX_TF_C4 || texformat == GX_TF_C8 || texformat == GX_TF_C14X2);
	if (isPaletteTexture)
		full_format = texformat | (tlutfmt << 16);

	u8* ptr = Memory::GetPointer(address);
	const u32 texture_size = TexDecoder_GetTextureSizeInBytes(expandedWidth, expandedHeight, texformat);

	hash_value = texHash = GetHash64(ptr, texture_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);
	if (isPaletteTexture)
	{
		const u32 palette_size = TexDecoder_GetPaletteSize(texformat);
		tlut_hash = GetHash64(&texMem[tlutaddr], palette_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);

		// NOTE: For non-paletted textures, texID is equal to the texture address.
		//		A paletted texture, however, may have multiple texIDs assigned though depending on the currently used tlut.
		//		This (changing texID depending on the tlut_hash) is a trick to get around
		//		an issue with Metroid Prime's fonts (it has multiple sets of fonts on each other
		//		stored in a single texture and uses the palette to make different characters
		//		visible or invisible. Thus, unless we want to recreate the textures for every drawn character,
		//		we must make sure that a paletted texture gets assigned multiple IDs for each tlut used.
		//
		// TODO: Because texID isn't always the same as the address now, CopyRenderTargetToTexture might be broken now
		texID ^= ((u32)tlut_hash) ^(u32)(tlut_hash >> 32);
		hash_value = texHash ^= tlut_hash;
	}

	TCacheEntryBase *entry = textures[texID];
	if (entry)
	{
		// 1. Calculate reference hash:
		// calculated from RAM texture data for normal textures. Hashes for paletted textures are modified by tlut_hash. 0 for virtual EFB copies.
		if (g_ActiveConfig.bCopyEFBToTexture && entry->IsEfbCopy())
			hash_value = TEXHASH_INVALID;

		// 2. a) For EFB copies, only the hash and the texture address need to match
		if (entry->IsEfbCopy() && hash_value == entry->hash && address == entry->addr)
		{
			// TODO: Print a warning if the format changes! In this case, we could reinterpret the internal texture object data to the new pixel format (similiar to what is already being done in Renderer::ReinterpretPixelFormat())
			goto return_entry;
		}

		// 2. b) For normal textures, all texture parameters need to match
		if (address == entry->addr && hash_value == entry->hash && full_format == entry->format &&
			entry->num_mipmaps == maxlevel && entry->native_width == nativeW && entry->native_height == nativeH)
		{
			goto return_entry;
		}

		// 3. If we reach this line, we'll have to upload the new texture data to VRAM.
		//    If we're lucky, the texture parameters didn't change and we can reuse the internal texture object instead of destroying and recreating it.
		//
		// TODO: Don't we need to force texture decoding to RGBA8 for dynamic EFB copies?
		// TODO: Actually, it should be enough if the internal texture format matches...
		if ((entry->efbcopy_state == EC_NO_COPY && width == entry->native_width && height == entry->native_height && full_format == entry->format && entry->num_mipmaps == maxlevel)
			|| (entry->efbcopy_state == EC_VRAM_DYNAMIC && entry->native_width == width && entry->native_height == height))
		{
			// reuse the texture
		}
		else
		{
			// delete the texture and make a new one
			delete entry;
			entry = NULL;
		}
	}

	if (g_ActiveConfig.bHiresTextures)
	{
		// Load Custom textures
		char texPathTemp[MAX_PATH];

		unsigned int newWidth = width;
		unsigned int newHeight = height;

		sprintf(texPathTemp, "%s_%08x_%i", SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str(), (u32) (texHash & 0x00000000FFFFFFFFLL), texformat);
		pcfmt = HiresTextures::GetHiresTex(texPathTemp, &newWidth, &newHeight, texformat, temp);

		if (pcfmt != PC_TEX_FMT_NONE)
		{
			expandedWidth = width = newWidth;
			expandedHeight = height = newHeight;
		}
	}

	if (pcfmt == PC_TEX_FMT_NONE)
		pcfmt = TexDecoder_Decode(temp, ptr, expandedWidth,
					expandedHeight, texformat, tlutaddr, tlutfmt, g_ActiveConfig.backend_info.bUseRGBATextures);

	bool isPow2;
	unsigned int texLevels;
	UseNativeMips = UseNativeMips && (width == nativeW && height == nativeH); // Only load native mips if their dimensions fit to our virtual texture dimensions
	isPow2 = !((width & (width - 1)) || (height & (height - 1)));
	texLevels = (isPow2 && UseNativeMips && maxlevel) ?
		GetPow2(std::max(width, height)) : !isPow2;

	if ((texLevels > (maxlevel + 1)) && maxlevel)
		texLevels = maxlevel + 1;

	// create the entry/texture
	if (NULL == entry) {
		textures[texID] = entry = g_texture_cache->CreateTexture(width, height, expandedWidth, texLevels, pcfmt);

		// Sometimes, we can get around recreating a texture if only the number of mip levels gets changes
		// e.g. if our texture cache entry got too many mipmap levels we can limit the number of used levels by setting the appropriate render states
		// Thus, we don't update this member for every Load, but just whenever the texture gets recreated
		//
		// TODO: Won't we end up recreating textures all the time because maxlevel doesn't necessarily equal texLevels?
		entry->num_mipmaps = maxlevel; // TODO: Does this actually work? We can't really adjust mipmap settings per-stage...
		entry->efbcopy_state = EC_NO_COPY;

		GFX_DEBUGGER_PAUSE_AT(NEXT_NEW_TEXTURE, true);
	}

	entry->SetGeneralParameters(address, texture_size, full_format, entry->num_mipmaps);
	entry->SetDimensions(nativeW, nativeH, width, height);
	entry->hash = hash_value;
	if (g_ActiveConfig.bCopyEFBToTexture) entry->efbcopy_state = EC_NO_COPY;
	else if (entry->IsEfbCopy()) entry->efbcopy_state = EC_VRAM_DYNAMIC;

	// load texture
	entry->Load(width, height, expandedWidth, 0, (texLevels == 0));

	// load mips
	if (texLevels > 1 && pcfmt != PC_TEX_FMT_NONE)
	{
		const unsigned int bsdepth = TexDecoder_GetTexelSizeInNibbles(texformat);

		unsigned int level = 1;
		unsigned int mipWidth = (width + 1) >> 1;
		unsigned int mipHeight = (height + 1) >> 1;
		ptr += texture_size;

		while ((mipHeight || mipWidth) && (level < texLevels))
		{
			const unsigned int currentWidth = (mipWidth > 0) ? mipWidth : 1;
			const unsigned int currentHeight = (mipHeight > 0) ? mipHeight : 1;

			expandedWidth  = (currentWidth + bsw)  & (~bsw);
			expandedHeight = (currentHeight + bsh) & (~bsh);

			TexDecoder_Decode(temp, ptr, expandedWidth, expandedHeight, texformat, tlutaddr, tlutfmt, g_ActiveConfig.backend_info.bUseRGBATextures);
			entry->Load(currentWidth, currentHeight, expandedWidth, level, false);

			ptr += ((std::max(mipWidth, bsw) * std::max(mipHeight, bsh) * bsdepth) >> 1);
			mipWidth >>= 1;
			mipHeight >>= 1;
			++level;
		}
	}

	// TODO: won't this cause loaded hires textures to be dumped as well?
	// dump texture to file
	if (g_ActiveConfig.bDumpTextures)
	{
		char szTemp[MAX_PATH];
		std::string szDir = File::GetUserPath(D_DUMPTEXTURES_IDX) +
			SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID;

		// make sure that the directory exists
		if (false == File::Exists(szDir) || false == File::IsDirectory(szDir))
			File::CreateDir(szDir.c_str());

		sprintf(szTemp, "%s/%s_%08x_%i.png", szDir.c_str(),
				SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str(),
				(u32) (texHash & 0x00000000FFFFFFFFLL), texformat);

		if (false == File::Exists(szTemp))
			entry->Save(szTemp);
	}

	INCSTAT(stats.numTexturesCreated);
	SETSTAT(stats.numTexturesAlive, textures.size());

return_entry:

	entry->frameCount = frameCount;
	entry->Bind(stage);

	GFX_DEBUGGER_PAUSE_AT(NEXT_TEXTURE_CHANGE, true);

	return entry;
}

void TextureCache::CopyRenderTargetToTexture(u32 dstAddr, unsigned int dstFormat, unsigned int srcFormat,
	const EFBRectangle& srcRect, bool isIntensity, bool scaleByHalf)
{
	// Emulation methods:
	// - EFB to RAM:
	//		Encodes the requested EFB data at its native resolution to the emulated RAM using shaders.
	//		Load() decodes the data from there again (using TextureDecoder) if the EFB copy is being used as a texture again.
	//		Advantage: CPU can read data from the EFB copy and we don't lose any important updates to the texture
	//		Disadvantage: Encoding+decoding steps often are redundant because only some games read or modify EFB copies before using them as textures.
	// - EFB to texture:
	//		Copies the requested EFB data to a texture object in VRAM, performing any color conversion using shaders.
	//		Advantage: Works for many games, since in most cases EFB copies aren't read or modified at all before being used as a texture again.
	//					Since we don't do any further encoding or decoding here, this method is much faster.
	//					It also allows enhancing the visual quality by doing scaled EFB copies.
	// - hybrid EFB copies:
	//		1) Whenever this function gets called, encode the requested EFB data to RAM (like EFB to RAM)
	//		2a) If we haven't copied to the specified dstAddr yet, copy the requested EFB data to a texture object in VRAM as well (like EFB to texture)
	//			Create a texture cache entry for the render target (isRenderTarget = true, isDynamic = false)
	//			Store a hash of the encoded RAM data in the texcache entry.
	//		2b) If we already have created a texcache entry for dstAddr (i.e. if we copied to dstAddr before) AND isDynamic is false:
	//			Do the same like above, but reuse the old texcache entry instead of creating a new one.
	//		2c) If we already have created a texcache entry for dstAddr AND isDynamic is true (isRenderTarget will be false then)
	//			Only encode the texture to RAM (like EFB to RAM) and store a hash of the encoded data in the existing texcache entry.
	//			Do NOT copy the requested EFB data to a VRAM object. Reason: the texture is dynamic, i.e. the CPU is modifying it. Storing a VRAM copy is useless, because we'd end up deleting it and reloading the data from RAM again anyway.
	//		3) If the EFB copy gets used as a texture, compare the source RAM hash with the hash you stored when encoding the EFB data to RAM.
	//		3a) If the two hashes match AND isDynamic is still false, reuse the VRAM copy you created
	//		3b) If the two hashes differ AND isDynamic is still false, screw your existing VRAM copy. Set isRenderTarget to false and isDynamic to true.
	//			Redecode the source RAM data to a VRAM object. The entry basically behaves like a normal texture now.
	//		3c) If isDynamic is true, treat the EFB copy like a normal texture.
	//		Advantage: Neither as fast as EFB to texture nor as slow as EFB to RAM, so it's a good compromise.
	//					Non-dynamic EFB copies can be visually enhanced like with EFB to texture.
	//					Compatibility ideally is as good as with EFB to RAM.
	//		Disadvantage: Depends on accurate texture hashing being enabled. However, with accurate hashing you end up being as slow as EFB to RAM anyway.
	//
	// Disadvantage of all methods: Calling this function requires the GPU to perform a pipeline flush which stalls any further CPU processing.

	float colmat[28] = {0};
	float *const fConstAdd = colmat + 16;
	float *const ColorMask = colmat + 20;
	ColorMask[0] = ColorMask[1] = ColorMask[2] = ColorMask[3] = 255.0f;
	ColorMask[4] = ColorMask[5] = ColorMask[6] = ColorMask[7] = 1.0f / 255.0f;
	unsigned int cbufid = -1;

	if (srcFormat == PIXELFMT_Z24)
	{
		switch (dstFormat)
		{
		case 0: // Z4
			colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1.0f;
			cbufid = 0;
			break;
		case 1: // Z8
		case 8: // Z8
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1.0f;
			cbufid = 1;
			break;

		case 3: // Z16
			colmat[1] = colmat[5] = colmat[9] = colmat[12] = 1.0f;
			cbufid = 24;
			break;

		case 11: // Z16 (reverse order)
			colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1.0f;
			cbufid = 2;
			break;

		case 6: // Z24X8
			colmat[0] = colmat[5] = colmat[10] = 1.0f;
			cbufid = 3;
			break;

		case 9: // Z8M
			colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1.0f;
			cbufid = 4;
			break;

		case 10: // Z8L
			colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1.0f;
			cbufid = 5;
			break;

		case 12: // Z16L - copy lower 16 depth bits
			// expected to be used as an IA8 texture (upper 8 bits stored as intensity, lower 8 bits stored as alpha)
			// Used e.g. in Zelda: Skyward Sword
			colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1.0f;
			cbufid = 6;
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy zbuf format: 0x%x", dstFormat);
			colmat[2] = colmat[5] = colmat[8] = 1.0f;
			cbufid = 7;
			break;
		}
	}
	else if (isIntensity) 
	{
		fConstAdd[0] = fConstAdd[1] = fConstAdd[2] = 16.0f/255.0f;
		switch (dstFormat) 
		{
		case 0: // I4
		case 1: // I8
		case 2: // IA4
		case 3: // IA8
		case 8: // I8
			// TODO - verify these coefficients
			colmat[0] = 0.257f; colmat[1] = 0.504f; colmat[2] = 0.098f;
			colmat[4] = 0.257f; colmat[5] = 0.504f; colmat[6] = 0.098f;
			colmat[8] = 0.257f; colmat[9] = 0.504f; colmat[10] = 0.098f;

			if (dstFormat < 2 || dstFormat == 8) 
			{
				colmat[12] = 0.257f; colmat[13] = 0.504f; colmat[14] = 0.098f;
				fConstAdd[3] = 16.0f/255.0f;
				if (dstFormat == 0)
				{
					ColorMask[0] = ColorMask[1] = ColorMask[2] = 15.0f;
					ColorMask[4] = ColorMask[5] = ColorMask[6] = 1.0f / 15.0f;
					cbufid = 8;
				}
				else
				{
					cbufid = 9;	
				}				
			}
			else// alpha
			{
				colmat[15] = 1;
				if (dstFormat == 2)
				{
					ColorMask[0] = ColorMask[1] = ColorMask[2] = ColorMask[3] = 15.0f;
					ColorMask[4] = ColorMask[5] = ColorMask[6] = ColorMask[7] = 1.0f / 15.0f;
					cbufid = 10;
				}
				else
				{
					cbufid = 11;
				}
				
			}
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy intensity format: 0x%x", dstFormat);
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 23;
			break;
		}
	}
	else
	{
		switch (dstFormat) 
		{
		case 0: // R4
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
			ColorMask[0] = 15.0f;
			ColorMask[4] = 1.0f / 15.0f;
			cbufid = 12;
			break;
		case 1: // R8
		case 8: // R8
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
			cbufid = 13;			
			break;

		case 2: // RA4
			colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1.0f;
			ColorMask[0] = ColorMask[3] = 15.0f;
			ColorMask[4] = ColorMask[7] = 1.0f / 15.0f;
			cbufid = 14;
			break;
		case 3: // RA8
			colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1.0f;
			cbufid = 15;
			break;

		case 7: // A8
			colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1.0f;
			cbufid = 16;
			break;

		case 9: // G8
			colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1.0f;
			cbufid = 17;			
			break;
		case 10: // B8
			colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1.0f;
			cbufid = 18;			
			break;

		case 11: // RG8
			colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1.0f;
			cbufid = 19;
			break;

		case 12: // GB8
			colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1.0f;			
			cbufid = 20;
			break;

		case 4: // RGB565
			colmat[0] = colmat[5] = colmat[10] = 1.0f;
			ColorMask[0] = ColorMask[2] = 31.0f;
			ColorMask[4] = ColorMask[6] = 1.0f / 31.0f;
			ColorMask[1] = 63.0f;
			ColorMask[5] = 1.0f / 63.0f;
			fConstAdd[3] = 1.0f; // set alpha to 1
			cbufid = 21;
			break;

		case 5: // RGB5A3
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			ColorMask[0] = ColorMask[1] = ColorMask[2] = 31.0f;
			ColorMask[4] = ColorMask[5] = ColorMask[6] = 1.0f / 31.0f;
			ColorMask[3] = 7.0f;
			ColorMask[7] = 1.0f / 7.0f;
			cbufid = 22;
			break;
		case 6: // RGBA8
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 23;
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy color format: 0x%x", dstFormat);
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 23;
			break;
		}
	}

	const unsigned int tex_w = scaleByHalf ? srcRect.GetWidth()/2 : srcRect.GetWidth();
	const unsigned int tex_h = scaleByHalf ? srcRect.GetHeight()/2 : srcRect.GetHeight();

	unsigned int scaled_tex_w = g_ActiveConfig.bCopyEFBScaled ? Renderer::EFBToScaledX(tex_w) : tex_w;
	unsigned int scaled_tex_h = g_ActiveConfig.bCopyEFBScaled ? Renderer::EFBToScaledY(tex_h) : tex_h;


	TCacheEntryBase *entry = textures[dstAddr];
	if (entry)
	{
		if ((entry->efbcopy_state == EC_VRAM_READY && entry->virtual_width == scaled_tex_w && entry->virtual_height == scaled_tex_h) 
			|| (entry->efbcopy_state == EC_VRAM_DYNAMIC && entry->native_width == tex_w && entry->native_height == tex_h))
		{
			scaled_tex_w = tex_w;
			scaled_tex_h = tex_h;
		}
		else
		{
			// remove it and recreate it as a render target
			delete entry;
			entry = NULL;
		}
	}

	if (NULL == entry)
	{
		// create the texture
		textures[dstAddr] = entry = g_texture_cache->CreateRenderTargetTexture(scaled_tex_w, scaled_tex_h);

		// TODO: Using the wrong dstFormat, dumb...
		entry->SetGeneralParameters(dstAddr, 0, dstFormat, 0);
		entry->SetDimensions(tex_w, tex_h, scaled_tex_w, scaled_tex_h);
		entry->SetHashes(TEXHASH_INVALID);
		entry->efbcopy_state = EC_VRAM_READY;
	}

	entry->frameCount = frameCount;

	g_renderer->ResetAPIState(); // reset any game specific settings

	entry->FromRenderTarget(dstAddr, dstFormat, srcFormat, srcRect, isIntensity, scaleByHalf, cbufid, colmat);

	g_renderer->RestoreAPIState();
}
