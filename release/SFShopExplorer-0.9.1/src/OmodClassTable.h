// ============================================================================
// OmodClassTable.h - GENERATED, do not edit
//
// Produced by tools/gen_omod_class_table.py from a dump of the running game. Regenerate it there
// rather than editing this file: the values are engine data, not design
// decisions, and a hand edit is invisible to the next regeneration.
// ============================================================================

#pragma once

#include <cstdint>

namespace SFE
{
	// Build-94 rarity tables generated from Starfield.esm by
	// tools/gen_omod_class_table.py. Quality OMODs carry no MNAM and
	// no model (so the MNAM census can never see them) and their
	// runtime editor IDs are unreadable; the file-side EDID
	// mod_<Weapon>_Quality_0N supplies both the weapon binding and
	// the rank. mod_Legendary_Wep_N_* OMODs are the universal weapon
	// effect pool (rank 1-3, no weapon binding, no AP).
	//
	// Armor effects are per class instead of one pool: mod_Legendary_Armor_0N_*
	// for suits/apparel, mod_Legendary_Helmet_0N_* for helmets and hats,
	// mod_Legendary_Pack_0N_* for backpacks. Same rank convention.
	// All tables are sorted by OMOD formID for binary search. Base game only.
	struct OmodQualityEntry
	{
		std::uint32_t omod;
		std::uint32_t weapon;
		std::uint32_t rank;   // 1=Common(01) 2=Calibrated 3=Refined 4=Advanced
	};

	struct OmodLegendaryEntry
	{
		std::uint32_t omod;
		std::uint32_t rank;   // 1..3
	};

	inline constexpr OmodQualityEntry kOmodQualityTable[] = {
		{ 0x00062886u, 0x00028A02u, 2u },
		{ 0x00062888u, 0x00028A02u, 3u },
		{ 0x0006288Cu, 0x00028A02u, 1u },
		{ 0x00062896u, 0x00028A02u, 4u },
		{ 0x0012E04Au, 0x0001BC4Fu, 1u },
		{ 0x0012E10Du, 0x0001BC4Fu, 2u },
		{ 0x0012E10Eu, 0x0001BC4Fu, 3u },
		{ 0x0012E3CCu, 0x002984DFu, 1u },
		{ 0x0012E3CFu, 0x002984DFu, 2u },
		{ 0x0012E9A1u, 0x002984DFu, 3u },
		{ 0x0012EA89u, 0x002984DFu, 4u },
		{ 0x0012EA8Eu, 0x0004716Cu, 1u },
		{ 0x0012EA93u, 0x0004716Cu, 2u },
		{ 0x0012EAABu, 0x0004716Cu, 3u },
		{ 0x0012EAACu, 0x0004716Cu, 4u },
		{ 0x0012EAAFu, 0x0018DE2Cu, 1u },
		{ 0x0012EAB0u, 0x0018DE2Cu, 2u },
		{ 0x0012EAC2u, 0x0018DE2Cu, 3u },
		{ 0x0012EAC6u, 0x0018DE2Cu, 4u },
		{ 0x0012F30Du, 0x00253A16u, 1u },
		{ 0x0012F30Eu, 0x00253A16u, 2u },
		{ 0x0012F310u, 0x00253A16u, 3u },
		{ 0x0012F313u, 0x00253A16u, 4u },
		{ 0x0012F316u, 0x0021FEB4u, 1u },
		{ 0x0012F330u, 0x0021FEB4u, 2u },
		{ 0x0012FB63u, 0x0021FEB4u, 3u },
		{ 0x0012FD1Fu, 0x0021FEB4u, 4u },
		{ 0x0012FD21u, 0x0026D961u, 1u },
		{ 0x0012FD23u, 0x0026D961u, 2u },
		{ 0x0012FD24u, 0x0026D961u, 3u },
		{ 0x0012FD25u, 0x0026D961u, 4u },
		{ 0x0012FD27u, 0x0026D96Bu, 1u },
		{ 0x0012FD28u, 0x0026D96Bu, 2u },
		{ 0x0012FD29u, 0x0026D96Bu, 3u },
		{ 0x0012FD2Au, 0x0026D96Bu, 4u },
		{ 0x0012FD2Cu, 0x002BF65Bu, 1u },
		{ 0x0012FD2Du, 0x002BF65Bu, 2u },
		{ 0x0012FD2Eu, 0x002BF65Bu, 3u },
		{ 0x0012FD2Fu, 0x002BF65Bu, 4u },
		{ 0x0012FD35u, 0x002953F8u, 1u },
		{ 0x0012FD36u, 0x002953F8u, 2u },
		{ 0x0012FD37u, 0x002953F8u, 3u },
		{ 0x0012FD38u, 0x002953F8u, 4u },
		{ 0x0012FD3Eu, 0x0026D960u, 1u },
		{ 0x0012FD3Fu, 0x0026D960u, 2u },
		{ 0x0012FD40u, 0x0026D960u, 3u },
		{ 0x0012FD41u, 0x0026D960u, 4u },
		{ 0x0012FD46u, 0x000546CCu, 1u },
		{ 0x0012FD47u, 0x000546CCu, 2u },
		{ 0x0012FD48u, 0x000546CCu, 3u },
		{ 0x0012FD49u, 0x000546CCu, 4u },
		{ 0x0012FD50u, 0x00023606u, 1u },
		{ 0x0012FD51u, 0x00023606u, 2u },
		{ 0x0012FD52u, 0x00023606u, 3u },
		{ 0x0012FD53u, 0x00023606u, 4u },
		{ 0x0012FD59u, 0x0002EB3Cu, 1u },
		{ 0x0012FD5Au, 0x0002EB3Cu, 2u },
		{ 0x0012FDE8u, 0x0002EB3Cu, 3u },
		{ 0x0012FDE9u, 0x0002EB3Cu, 4u },
		{ 0x0012FFABu, 0x0002EB42u, 1u },
		{ 0x001301E7u, 0x0002EB42u, 2u },
		{ 0x001301E8u, 0x0002EB42u, 3u },
		{ 0x001301E9u, 0x0002EB42u, 4u },
		{ 0x001301EEu, 0x0026035Eu, 1u },
		{ 0x001301EFu, 0x0026035Eu, 2u },
		{ 0x001301F0u, 0x0026035Eu, 3u },
		{ 0x001308D4u, 0x0026035Eu, 4u },
		{ 0x00131574u, 0x0002EB45u, 1u },
		{ 0x00131575u, 0x0002EB45u, 2u },
		{ 0x00131BFBu, 0x0002EB45u, 3u },
		{ 0x00131D43u, 0x0002EB45u, 4u },
		{ 0x00131D48u, 0x0026D963u, 1u },
		{ 0x00131D49u, 0x0026D963u, 2u },
		{ 0x00131D4Au, 0x0026D963u, 3u },
		{ 0x00131D4Bu, 0x0026D963u, 4u },
		{ 0x001BDC25u, 0x002773C8u, 1u },
		{ 0x001BDC26u, 0x002773C8u, 2u },
		{ 0x001BDC27u, 0x002773C8u, 3u },
		{ 0x001BDC28u, 0x002773C8u, 4u },
		{ 0x001BDC2Du, 0x0026D95Du, 1u },
		{ 0x001BDC2Eu, 0x0026D95Du, 2u },
		{ 0x001BDC2Fu, 0x0026D95Du, 3u },
		{ 0x001BDC34u, 0x0026D96Du, 1u },
		{ 0x001BDC35u, 0x0026D96Du, 2u },
		{ 0x001BDC36u, 0x0026D96Du, 3u },
		{ 0x001BDC37u, 0x0026D96Du, 4u },
		{ 0x001BDC3Cu, 0x000546CDu, 1u },
		{ 0x001BDC3Du, 0x000546CDu, 2u },
		{ 0x001BDC3Eu, 0x000546CDu, 3u },
		{ 0x001BDC3Fu, 0x000546CDu, 4u },
		{ 0x001BDC44u, 0x0026D970u, 1u },
		{ 0x001BDC45u, 0x0026D970u, 2u },
		{ 0x001BDC46u, 0x0026D970u, 3u },
		{ 0x001BDC47u, 0x0026D970u, 4u },
		{ 0x0028F439u, 0x0026D967u, 1u },
		{ 0x0028F43Au, 0x0026D967u, 2u },
		{ 0x0028F43Bu, 0x0026D967u, 3u },
		{ 0x0028F43Cu, 0x0026D967u, 4u },
		{ 0x0028F441u, 0x000547A3u, 1u },
		{ 0x0028F442u, 0x000547A3u, 2u },
		{ 0x0028F443u, 0x000547A3u, 3u },
		{ 0x0028F444u, 0x000547A3u, 4u },
		{ 0x0028F448u, 0x0026D96Au, 1u },
		{ 0x0028F449u, 0x0026D96Au, 2u },
		{ 0x0028F44Au, 0x0026D96Au, 3u },
		{ 0x0028F44Bu, 0x0026D96Au, 4u },
		{ 0x0028F450u, 0x000476C4u, 1u },
		{ 0x0028F451u, 0x000476C4u, 2u },
		{ 0x0028F452u, 0x000476C4u, 3u },
		{ 0x0028F457u, 0x0026D8A0u, 1u },
		{ 0x0028F458u, 0x0026D8A0u, 2u },
		{ 0x0028F459u, 0x0026D8A0u, 3u },
		{ 0x0028F45Au, 0x0026D8A0u, 4u },
		{ 0x0028F45Fu, 0x0026D8A4u, 1u },
		{ 0x0028F460u, 0x0026D8A4u, 2u },
		{ 0x0028F461u, 0x0026D8A4u, 3u },
		{ 0x0028F462u, 0x0026D8A4u, 4u },
		{ 0x0028FC01u, 0x0002D7F4u, 1u },
		{ 0x0028FC02u, 0x0002D7F4u, 2u },
		{ 0x0028FC03u, 0x0002D7F4u, 3u },
		{ 0x0028FC04u, 0x0002D7F4u, 4u },
		{ 0x0028FC08u, 0x0026D96Cu, 1u },
		{ 0x0028FC09u, 0x0026D96Cu, 2u },
		{ 0x0028FC0Au, 0x0026D96Cu, 3u },
		{ 0x0028FC0Bu, 0x0026D96Cu, 4u },
		{ 0x0028FC0Fu, 0x00278F74u, 1u },
		{ 0x0028FC10u, 0x00278F74u, 2u },
		{ 0x0028FC11u, 0x00278F74u, 3u },
		{ 0x0028FC12u, 0x00278F74u, 4u },
		{ 0x0028FC17u, 0x0026ED2Au, 1u },
		{ 0x0028FC18u, 0x0026ED2Au, 2u },
		{ 0x0028FC19u, 0x0026ED2Au, 3u },
		{ 0x0028FC1Au, 0x0026ED2Au, 4u },
		{ 0x0028FC1Du, 0x0021BBCDu, 1u },
		{ 0x0028FC1Eu, 0x0021BBCDu, 2u },
		{ 0x0028FC1Fu, 0x0021BBCDu, 3u },
		{ 0x0028FC20u, 0x0021BBCDu, 4u },
		{ 0x0028FC25u, 0x0002EB36u, 1u },
		{ 0x0028FC26u, 0x0002EB36u, 2u },
		{ 0x0028FC27u, 0x0002EB36u, 3u },
		{ 0x0028FC28u, 0x0002EB36u, 4u },
		{ 0x0028FC2Cu, 0x0024561Cu, 1u },
		{ 0x0028FC2Du, 0x0024561Cu, 2u },
		{ 0x0028FC2Eu, 0x0024561Cu, 3u },
		{ 0x0028FC2Fu, 0x0024561Cu, 4u },
		{ 0x0028FC33u, 0x00040826u, 1u },
		{ 0x0028FC34u, 0x00040826u, 2u },
		{ 0x0028FC37u, 0x00040826u, 3u },
		{ 0x0028FC3Au, 0x0002CB5Fu, 1u },
		{ 0x0028FC3Bu, 0x0002CB5Fu, 2u },
		{ 0x0028FC3Cu, 0x0002CB5Fu, 3u },
		{ 0x0028FC3Du, 0x0002CB5Fu, 4u },
		{ 0x0028FC41u, 0x00000FD6u, 1u },
		{ 0x0028FC42u, 0x00000FD6u, 2u },
		{ 0x0028FC43u, 0x00000FD6u, 3u },
		{ 0x0028FC44u, 0x00000FD6u, 4u },
		{ 0x002EF2B8u, 0x0026F181u, 1u },
		{ 0x002EF2B9u, 0x0026F181u, 2u },
		{ 0x002EF2BAu, 0x0026F181u, 3u },
		{ 0x002EF2BBu, 0x0026F181u, 4u },
		{ 0x002EF2BDu, 0x00035A48u, 1u },
		{ 0x002EF2BEu, 0x00035A48u, 2u },
		{ 0x002EF2BFu, 0x00035A48u, 3u },
		{ 0x002EF2C0u, 0x00035A48u, 4u },
		{ 0x002EF2C2u, 0x0004F760u, 1u },
		{ 0x002EF2C3u, 0x0004F760u, 2u },
		{ 0x002EF2C8u, 0x0004F760u, 3u },
		{ 0x002EF307u, 0x0004F760u, 4u },
		{ 0x002EF30Bu, 0x0026D8A5u, 1u },
		{ 0x002EF33Au, 0x0026D8A5u, 2u },
		{ 0x002EF33Eu, 0x0026D8A5u, 3u },
		{ 0x002EF340u, 0x0026D8A5u, 4u },
		{ 0x002EF3B8u, 0x0026D966u, 1u },
		{ 0x002EF3BDu, 0x0026D966u, 2u },
		{ 0x002EF3C8u, 0x0026D966u, 3u },
		{ 0x002EF3C9u, 0x0026D966u, 4u },
		{ 0x002EF3CBu, 0x0026D8A3u, 1u },
		{ 0x002EF3CCu, 0x0026D8A3u, 2u },
		{ 0x002EF3CDu, 0x0026D8A3u, 3u },
		{ 0x002EF3CEu, 0x0026D8A3u, 4u },
		{ 0x002EF3EDu, 0x0026D8A1u, 1u },
		{ 0x002EF3EEu, 0x0026D8A1u, 2u },
		{ 0x002EF50Du, 0x0026D8A1u, 3u },
		{ 0x002EF516u, 0x0026D8A1u, 4u },
		{ 0x002EF616u, 0x0026D8A2u, 1u },
		{ 0x002EF617u, 0x0026D8A2u, 2u },
		{ 0x002EF618u, 0x0026D8A2u, 3u },
		{ 0x002EF74Eu, 0x0026D8A2u, 4u },
		{ 0x002EF9ABu, 0x0026D95Eu, 1u },
		{ 0x002EF9ACu, 0x0026D95Eu, 2u },
		{ 0x002EF9ADu, 0x0026D95Eu, 3u },
		{ 0x002EF9B6u, 0x0026D95Eu, 4u },
		{ 0x003197CCu, 0x0026D965u, 1u },
		{ 0x003197CDu, 0x0026D965u, 2u },
		{ 0x003197CEu, 0x0026D965u, 3u },
		{ 0x003197CFu, 0x0026D965u, 4u },
		{ 0x003197D0u, 0x0026D964u, 1u },
		{ 0x003197D1u, 0x0026D964u, 2u },
		{ 0x003197D2u, 0x0026D964u, 3u },
		{ 0x003197D3u, 0x0026D964u, 4u },
		{ 0x003198FFu, 0x000476C4u, 4u },
		{ 0x00319924u, 0x0001BC4Fu, 4u },
		{ 0x0031992Eu, 0x00040826u, 4u },
		{ 0x00319951u, 0x0026D95Du, 4u },
	};

	inline constexpr OmodLegendaryEntry kOmodLegendaryTable[] = {
		{ 0x0007D728u, 2u },
		{ 0x0008AB47u, 2u },
		{ 0x000E8D64u, 2u },
		{ 0x000EA0BAu, 2u },
		{ 0x000EA117u, 1u },
		{ 0x000EA13Bu, 2u },
		{ 0x000F2013u, 1u },
		{ 0x000F2E39u, 2u },
		{ 0x000F428Eu, 1u },
		{ 0x000F437Eu, 1u },
		{ 0x000F4557u, 3u },
		{ 0x000F4CF0u, 3u },
		{ 0x000F7321u, 1u },
		{ 0x000FA8D6u, 3u },
		{ 0x000FAEABu, 1u },
		{ 0x000FBD3Cu, 3u },
		{ 0x000FC884u, 3u },
		{ 0x000FC8A4u, 3u },
		{ 0x000FEA04u, 2u },
		{ 0x000FEA07u, 1u },
		{ 0x000FEA49u, 2u },
		{ 0x000FF442u, 1u },
		{ 0x000FFA3Bu, 1u },
		{ 0x000FFA3Cu, 2u },
		{ 0x000FFA3Du, 3u },
		{ 0x00122F1Cu, 2u },
		{ 0x0015DD18u, 1u },
		{ 0x001625EBu, 1u },
		{ 0x002F97DCu, 2u },
		{ 0x002F97DFu, 1u },
		{ 0x002F97E1u, 2u },
		{ 0x0031996Bu, 1u },
		{ 0x00319AECu, 2u },
		{ 0x0031C0C4u, 3u },
		{ 0x0031C0C5u, 3u },
		{ 0x0031C0C6u, 3u },
	};

	inline const OmodQualityEntry* LookupOmodQuality(std::uint32_t a_omod)
	{
		std::uint32_t lo = 0;
		std::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodQualityTable) / sizeof(kOmodQualityTable[0]));
		while (lo < hi) {
			const auto mid = lo + (hi - lo) / 2;
			if (kOmodQualityTable[mid].omod < a_omod) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (lo < sizeof(kOmodQualityTable) / sizeof(kOmodQualityTable[0]) && kOmodQualityTable[lo].omod == a_omod) {
			return &kOmodQualityTable[lo];
		}
		return nullptr;
	}

	struct OmodArmorLegendaryEntry
	{
		std::uint32_t omod;
		std::uint32_t slot;   // 0 = suit/apparel body, 1 = helmet/hat, 2 = backpack
		std::uint32_t rank;   // 1..3
	};

	inline constexpr OmodArmorLegendaryEntry kOmodArmorLegendaryTable[] = {
		{ 0x00002983u, 0u, 3u },
		{ 0x00059AE8u, 0u, 3u },
		{ 0x00060293u, 0u, 2u },
		{ 0x00060295u, 0u, 2u },
		{ 0x0006029Du, 0u, 3u },
		{ 0x0006029Fu, 0u, 3u },
		{ 0x000690AEu, 1u, 1u },
		{ 0x000690AFu, 1u, 2u },
		{ 0x000690B0u, 1u, 1u },
		{ 0x000710F5u, 0u, 2u },
		{ 0x000710F6u, 0u, 2u },
		{ 0x000710F7u, 0u, 2u },
		{ 0x000710FAu, 0u, 2u },
		{ 0x000710FDu, 0u, 2u },
		{ 0x000BE540u, 0u, 3u },
		{ 0x000BE542u, 0u, 3u },
		{ 0x000C9A43u, 0u, 2u },
		{ 0x00133699u, 0u, 1u },
		{ 0x0013369Cu, 0u, 1u },
		{ 0x0013369Eu, 0u, 1u },
		{ 0x001336BCu, 0u, 1u },
		{ 0x001336BDu, 0u, 1u },
		{ 0x001336BEu, 0u, 1u },
		{ 0x001336C1u, 0u, 1u },
		{ 0x001336C6u, 0u, 1u },
		{ 0x002C43DAu, 1u, 2u },
		{ 0x002C43DBu, 1u, 3u },
		{ 0x002C43DCu, 1u, 3u },
		{ 0x002D01A2u, 0u, 3u },
		{ 0x002EDE4Eu, 2u, 2u },
		{ 0x002EDE4Fu, 2u, 3u },
		{ 0x002EDE59u, 2u, 3u },
	};

	inline const OmodLegendaryEntry* LookupOmodLegendary(std::uint32_t a_omod)
	{
		std::uint32_t lo = 0;
		std::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodLegendaryTable) / sizeof(kOmodLegendaryTable[0]));
		while (lo < hi) {
			const auto mid = lo + (hi - lo) / 2;
			if (kOmodLegendaryTable[mid].omod < a_omod) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (lo < sizeof(kOmodLegendaryTable) / sizeof(kOmodLegendaryTable[0]) && kOmodLegendaryTable[lo].omod == a_omod) {
			return &kOmodLegendaryTable[lo];
		}
		return nullptr;
	}

	inline const OmodArmorLegendaryEntry* LookupOmodArmorLegendary(std::uint32_t a_omod)
	{
		std::uint32_t lo = 0;
		std::uint32_t hi = static_cast<std::uint32_t>(sizeof(kOmodArmorLegendaryTable) / sizeof(kOmodArmorLegendaryTable[0]));
		while (lo < hi) {
			const auto mid = lo + (hi - lo) / 2;
			if (kOmodArmorLegendaryTable[mid].omod < a_omod) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (lo < sizeof(kOmodArmorLegendaryTable) / sizeof(kOmodArmorLegendaryTable[0]) && kOmodArmorLegendaryTable[lo].omod == a_omod) {
			return &kOmodArmorLegendaryTable[lo];
		}
		return nullptr;
	}
}
