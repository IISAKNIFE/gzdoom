/*
** p_saveg.cpp
** Code for serializing the world state in an archive
**
**---------------------------------------------------------------------------
** Copyright 1998-2016 Randy Heit
** Copyright 2005-2016 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "i_system.h"
#include "p_local.h"
#include "p_spec.h"

// State.
#include "d_player.h"
#include "dobject.h"
#include "doomstat.h"
#include "r_state.h"
#include "m_random.h"
#include "p_saveg.h"
#include "s_sndseq.h"
#include "v_palette.h"
#include "a_sharedglobal.h"
#include "r_data/r_interpolate.h"
#include "g_level.h"
#include "po_man.h"
#include "p_setup.h"
#include "r_data/colormaps.h"
#include "farchive.h"
#include "p_lnspec.h"
#include "p_acs.h"
#include "p_terrain.h"
#include "am_map.h"
#include "r_data/r_translate.h"
#include "sbar.h"
#include "r_utility.h"
#include "r_sky.h"
#include "r_renderer.h"
#include "serializer.h"

// just the stuff that already got converted to FSerializer so that it can be seen as 'done' when searching.
#include "zzz_old.cpp"

void CopyPlayer (player_t *dst, player_t *src, const char *name);
static void ReadOnePlayer (FArchive &arc, bool skipload);
static void ReadMultiplePlayers (FArchive &arc, int numPlayers, int numPlayersNow, bool skipload);
static void SpawnExtraPlayers ();

//
// P_ArchivePlayers
//
void P_SerializePlayers (FArchive &arc, bool skipload)
{
	BYTE numPlayers, numPlayersNow;
	int i;

	// Count the number of players present right now.
	for (numPlayersNow = 0, i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i])
		{
			++numPlayersNow;
		}
	}

	if (arc.IsStoring())
	{
		// Record the number of players in this save.
		arc << numPlayersNow;

		// Record each player's name, followed by their data.
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			if (playeringame[i])
			{
				arc.WriteString (players[i].userinfo.GetName());
				players[i].Serialize (arc);
			}
		}
	}
	else
	{
		arc << numPlayers;

		// If there is only one player in the game, they go to the
		// first player present, no matter what their name.
		if (numPlayers == 1)
		{
			ReadOnePlayer (arc, skipload);
		}
		else
		{
			ReadMultiplePlayers (arc, numPlayers, numPlayersNow, skipload);
		}
		if (!skipload && numPlayersNow > numPlayers)
		{
			SpawnExtraPlayers ();
		}
		// Redo pitch limits, since the spawned player has them at 0.
		players[consoleplayer].SendPitchLimits();
	}
}

static void ReadOnePlayer (FArchive &arc, bool skipload)
{
	int i;
	char *name = NULL;
	bool didIt = false;

	arc << name;

	for (i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i])
		{
			if (!didIt)
			{
				didIt = true;
				player_t playerTemp;
				playerTemp.Serialize (arc);
				if (!skipload)
				{
					CopyPlayer (&players[i], &playerTemp, name);
				}
			}
			else
			{
				if (players[i].mo != NULL)
				{
					players[i].mo->Destroy();
					players[i].mo = NULL;
				}
			}
		}
	}
	delete[] name;
}

static void ReadMultiplePlayers (FArchive &arc, int numPlayers, int numPlayersNow, bool skipload)
{
	// For two or more players, read each player into a temporary array.
	int i, j;
	char **nametemp = new char *[numPlayers];
	player_t *playertemp = new player_t[numPlayers];
	BYTE *tempPlayerUsed = new BYTE[numPlayers];
	BYTE playerUsed[MAXPLAYERS];

	for (i = 0; i < numPlayers; ++i)
	{
		nametemp[i] = NULL;
		arc << nametemp[i];
		playertemp[i].Serialize (arc);
		tempPlayerUsed[i] = 0;
	}
	for (i = 0; i < MAXPLAYERS; ++i)
	{
		playerUsed[i] = playeringame[i] ? 0 : 2;
	}

	if (!skipload)
	{
		// Now try to match players from the savegame with players present
		// based on their names. If two players in the savegame have the
		// same name, then they are assigned to players in the current game
		// on a first-come, first-served basis.
		for (i = 0; i < numPlayers; ++i)
		{
			for (j = 0; j < MAXPLAYERS; ++j)
			{
				if (playerUsed[j] == 0 && stricmp(players[j].userinfo.GetName(), nametemp[i]) == 0)
				{ // Found a match, so copy our temp player to the real player
					Printf ("Found player %d (%s) at %d\n", i, nametemp[i], j);
					CopyPlayer (&players[j], &playertemp[i], nametemp[i]);
					playerUsed[j] = 1;
					tempPlayerUsed[i] = 1;
					break;
				}
			}
		}

		// Any players that didn't have matching names are assigned to existing
		// players on a first-come, first-served basis.
		for (i = 0; i < numPlayers; ++i)
		{
			if (tempPlayerUsed[i] == 0)
			{
				for (j = 0; j < MAXPLAYERS; ++j)
				{
					if (playerUsed[j] == 0)
					{
						Printf ("Assigned player %d (%s) to %d (%s)\n", i, nametemp[i], j, players[j].userinfo.GetName());
						CopyPlayer (&players[j], &playertemp[i], nametemp[i]);
						playerUsed[j] = 1;
						tempPlayerUsed[i] = 1;
						break;
					}
				}
			}
		}

		// Make sure any extra players don't have actors spawned yet. Happens if the players
		// present now got the same slots as they had in the save, but there are not as many
		// as there were in the save.
		for (j = 0; j < MAXPLAYERS; ++j)
		{
			if (playerUsed[j] == 0)
			{
				if (players[j].mo != NULL)
				{
					players[j].mo->Destroy();
					players[j].mo = NULL;
				}
			}
		}

		// Remove any temp players that were not used. Happens if there are fewer players
		// than there were in the save, and they got shuffled.
		for (i = 0; i < numPlayers; ++i)
		{
			if (tempPlayerUsed[i] == 0)
			{
				playertemp[i].mo->Destroy();
				playertemp[i].mo = NULL;
			}
		}
	}

	delete[] tempPlayerUsed;
	delete[] playertemp;
	for (i = 0; i < numPlayers; ++i)
	{
		delete[] nametemp[i];
	}
	delete[] nametemp;
}

void CopyPlayer (player_t *dst, player_t *src, const char *name)
{
	// The userinfo needs to be saved for real players, but it
	// needs to come from the save for bots.
	userinfo_t uibackup;
	userinfo_t uibackup2;

	uibackup.TransferFrom(dst->userinfo);
	uibackup2.TransferFrom(src->userinfo);

	int chasecam = dst->cheats & CF_CHASECAM;	// Remember the chasecam setting
	bool attackdown = dst->attackdown;
	bool usedown = dst->usedown;

	
	*dst = *src;		// To avoid memory leaks at this point the userinfo in src must be empty which is taken care of by the TransferFrom call above.

	dst->cheats |= chasecam;

	if (dst->Bot != nullptr)
	{
		botinfo_t *thebot = bglobal.botinfo;
		while (thebot && stricmp (name, thebot->name))
		{
			thebot = thebot->next;
		}
		if (thebot)
		{
			thebot->inuse = BOTINUSE_Yes;
		}
		bglobal.botnum++;
		dst->userinfo.TransferFrom(uibackup2);
	}
	else
	{
		dst->userinfo.TransferFrom(uibackup);
	}
	// Validate the skin
	dst->userinfo.SkinNumChanged(R_FindSkin(skins[dst->userinfo.GetSkin()].name, dst->CurrentPlayerClass));

	// Make sure the player pawn points to the proper player struct.
	if (dst->mo != nullptr)
	{
		dst->mo->player = dst;
	}

	// Same for the psprites.
	DPSprite *pspr = dst->psprites;
	while (pspr)
	{
		pspr->Owner = dst;

		pspr = pspr->Next;
	}

	// Don't let the psprites be destroyed when src is destroyed.
	src->psprites = nullptr;

	// These 2 variables may not be overwritten.
	dst->attackdown = attackdown;
	dst->usedown = usedown;
}

static void SpawnExtraPlayers ()
{
	// If there are more players now than there were in the savegame,
	// be sure to spawn the extra players.
	int i;

	if (deathmatch)
	{
		return;
	}

	for (i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].mo == NULL)
		{
			players[i].playerstate = PST_ENTER;
			P_SpawnPlayer(&playerstarts[i], i, (level.flags2 & LEVEL2_PRERAISEWEAPON) ? SPF_WEAPONFULLYUP : 0);
		}
	}
}



//
// Thinkers
//

//
// P_ArchiveThinkers
//

void P_SerializeThinkers (FArchive &arc, bool hubLoad)
{
	arc.EnableThinkers();
	DImpactDecal::SerializeTime (arc);
	DThinker::SerializeAll (arc, hubLoad);
}

void P_DestroyThinkers(bool hubLoad)
{
	if (hubLoad)
		DThinker::DestroyMostThinkers();
	else
		DThinker::DestroyAllThinkers();
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, line_t &line, line_t *def)
{
	if (arc.BeginObject(key))
	{
		arc("flags", line.flags, def->flags)
			("activation", line.activation, def->activation)
			("special", line.special, def->special)
			("alpha", line.alpha, def->alpha)
			.Args("args", line.args, def->args, line.special)
			("portalindex", line.portalindex, def->portalindex)
			// Unless the map loader is changed the sidedef references will not change between map loads so there's no need to save them.
			//.Array("sides", line.sidedef, 2)
			.EndObject();
	}
	return arc;

}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, side_t::part &part, side_t::part *def)
{
	if (arc.canSkip() && def != nullptr && !memcmp(&part, def, sizeof(part)))
	{
		return arc;
	}

	if (arc.BeginObject(key))
	{
		arc("xoffset", part.xOffset, def->xOffset)
			("yoffset", part.yOffset, def->yOffset)
			("xscale", part.xScale, def->xScale)
			("yscale", part.yScale, def->yScale)
			("texture", part.texture, def->texture)
			("interpolation", part.interpolation)
			.EndObject();
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, side_t &side, side_t *def)
{
	if (arc.BeginObject(key))
	{
		arc.Array("textures", side.textures, def->textures, 3, true)
			("light", side.Light, def->Light)
			("flags", side.Flags, def->Flags)
			// These also remain identical across map loads
			//("leftside", side.LeftSide)
			//("rightside", side.RightSide)
			//("index", side.Index)
			("attacheddecals", side.AttachedDecals)
			.EndObject();
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FLinkedSector &ls, FLinkedSector *def)
{
	if (arc.BeginObject(key))
	{
		arc("sector", ls.Sector)
			("type", ls.Type)
			.EndObject();
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, sector_t::splane &p, sector_t::splane *def)
{
	if (arc.canSkip() && def != nullptr && !memcmp(&p, def, sizeof(p)))
	{
		return arc;
	}

	if (arc.BeginObject(key))
	{
		arc("xoffs", p.xform.xOffs, def->xform.xOffs)
			("yoffs", p.xform.yOffs, def->xform.yOffs)
			("xscale", p.xform.xScale, def->xform.xScale)
			("yscale", p.xform.yScale, def->xform.yScale)
			("angle", p.xform.Angle, def->xform.Angle)
			("baseyoffs", p.xform.baseyOffs, def->xform.baseyOffs)
			("baseangle", p.xform.baseAngle, def->xform.baseAngle)
			("flags", p.Flags, def->Flags)
			("light", p.Light, def->Light)
			("texture", p.Texture, def->Texture)
			("texz", p.TexZ, def->TexZ)
			("alpha", p.alpha, def->alpha)
			.EndObject();
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, secplane_t &p, secplane_t *def)
{
	if (arc.canSkip() && def != nullptr && !memcmp(&p, def, sizeof(p)))
	{
		return arc;
	}

	if (arc.BeginObject(key))
	{
		arc("normal", p.normal, def->normal)
			("d", p.D, def->D)
			.EndObject();

		if (arc.isReading() && p.normal.Z != 0)
		{
			p.negiC = 1 / p.normal.Z;
		}
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, sector_t &p, sector_t *def)
{
	if (arc.BeginObject(key))
	{
		arc("floorplane", p.floorplane, def->floorplane)
			("ceilingplane", p.ceilingplane, def->ceilingplane)
			("lightlevel", p.lightlevel, def->lightlevel)
			("special", p.special, def->special)
			("soundtraversed", p.soundtraversed, def->soundtraversed)
			("seqtype", p.seqType, def->seqType)
			("seqname", p.SeqName, def->SeqName)
			("friction", p.friction, def->friction)
			("movefactor", p.movefactor, def->movefactor)
			("stairlock", p.stairlock, def->stairlock)
			("prevsec", p.prevsec, def->prevsec)
			("nextsec", p.nextsec, def->nextsec)
			.Array("planes", p.planes, def->planes, 2, true)
			// These cannot change during play.
			//("heightsec", p.heightsec)
			//("bottommap", p.bottommap)
			//("midmap", p.midmap)
			//("topmap", p.topmap)
			("damageamount", p.damageamount, def->damageamount)
			("damageinterval", p.damageinterval, def->damageinterval)
			("leakydamage", p.leakydamage, def->leakydamage)
			("damagetype", p.damagetype, def->damagetype)
			("sky", p.sky, def->sky)
			("moreflags", p.MoreFlags, def->MoreFlags)
			("flags", p.Flags, def->Flags)
			.Array("portals", p.Portals, def->Portals, 2, true)
			("zonenumber", p.ZoneNumber, def->ZoneNumber)
			.Array("interpolations", p.interpolations, 4, true)
			("soundtarget", p.SoundTarget)
			("secacttarget", p.SecActTarget)
			("floordata", p.floordata)
			("ceilingdata", p.ceilingdata)
			("lightingdata", p.lightingdata)
			("fakefloor_sectors", p.e->FakeFloor.Sectors)
			("midtexf_lines", p.e->Midtex.Floor.AttachedLines)
			("midtexf_sectors", p.e->Midtex.Floor.AttachedSectors)
			("midtexc_lines", p.e->Midtex.Ceiling.AttachedLines)
			("midtexc_sectors", p.e->Midtex.Ceiling.AttachedSectors)
			("linked_floor", p.e->Linked.Floor.Sectors)
			("linked_ceiling", p.e->Linked.Ceiling.Sectors)
			("colormap", p.ColorMap, def->ColorMap)
			.Terrain("floorterrain", p.terrainnum[0], &def->terrainnum[0])
			.Terrain("ceilingterrain", p.terrainnum[1], &def->terrainnum[1])
			.EndObject();
	}
	return arc;
}

//==========================================================================
//
// RecalculateDrawnSubsectors
//
// In case the subsector data is unusable this function tries to reconstruct
// if from the linedefs' ML_MAPPED info.
//
//==========================================================================

void RecalculateDrawnSubsectors()
{
	for (int i = 0; i<numsubsectors; i++)
	{
		subsector_t *sub = &subsectors[i];
		for (unsigned int j = 0; j<sub->numlines; j++)
		{
			if (sub->firstline[j].linedef != NULL &&
				(sub->firstline[j].linedef->flags & ML_MAPPED))
			{
				sub->flags |= SSECF_DRAWN;
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, subsector_t *&ss, subsector_t **)
{
	BYTE by;
	const char *str;

	if (arc.isWriting())
	{
		if (hasglnodes)
		{
			TArray<char> encoded(1 + (numsubsectors + 5) / 6);
			int p = 0;
			for (int i = 0; i < numsubsectors; i += 6)
			{
				by = 0;
				for (int j = 0; j < 6; j++)
				{
					if (i + j < numsubsectors && (subsectors[i + j].flags & SSECF_DRAWN))
					{
						by |= (1 << j);
					}
				}
				if (by < 10) by += '0';
				else if (by < 36) by += 'A' - 10;
				else if (by < 62) by += 'a' - 36;
				else if (by == 62) by = '-';
				else if (by == 63) by = '+';
				encoded[p++] = by;
			}
			encoded[p] = 0;
			str = &encoded[0];
			if (arc.BeginArray(key))
			{
				arc(nullptr, numvertexes)
					(nullptr, numsubsectors)
					.StringPtr(nullptr, str)
					.EndArray();
			}
		}
	}
	else
	{
		int num_verts, num_subs;
		bool success = false;
		if (arc.BeginArray(key))
		{
			arc(nullptr, num_verts)
				(nullptr, num_subs)
				.StringPtr(nullptr, str)
				.EndArray();

			if (num_verts == numvertexes && num_subs == numsubsectors && hasglnodes)
			{
				success = true;
				for (int i = 0; str[i] != 0; i++)
				{
					by = str[i];
					if (by >= '0' && by <= '9') by -= '0';
					else if (by >= 'A' && by <= 'Z') by -= 'A' - 10;
					else if (by >= 'a' && by <= 'z') by -= 'a' - 36;
					else if (by == '-') by = 62;
					else if (by == '+') by = 63;
					else
					{
						success = false;
						break;
					}
				}
			}
			if (hasglnodes && !success)
			{
				RecalculateDrawnSubsectors();
			}
		}

	}
	return arc;
}

//============================================================================
//
// Save a line portal for savegames.
//
//============================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FLinePortal &port, FLinePortal *def)
{
	if (arc.BeginObject(key))
	{
		arc("origin", port.mOrigin)
			("destination", port.mDestination)
			("displacement", port.mDisplacement)
			("type", port.mType)
			("flags", port.mFlags)
			("defflags", port.mDefFlags)
			("align", port.mAlign)
			.EndObject();
	}
	return arc;
}

//============================================================================
//
// Save a sector portal for savegames.
//
//============================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, FSectorPortal &port, FSectorPortal *def)
{
	if (arc.BeginObject(key))
	{
		arc("type", port.mType)
			("flags", port.mFlags)
			("partner", port.mPartner)
			("plane", port.mPlane)
			("origin", port.mOrigin)
			("destination", port.mDestination)
			("displacement", port.mDisplacement)
			("planez", port.mPlaneZ)
			("skybox", port.mSkybox)
			.EndObject();
	}
	return arc;
}

//==========================================================================
//
//
//
//==========================================================================

FSerializer &Serialize(FSerializer &arc, const char *key, ReverbContainer *&c, ReverbContainer **def)
{
	int id = (arc.isReading() || c == nullptr) ? 0 : c->ID;
	Serialize(arc, key, id, nullptr);
	if (arc.isReading())
	{
		c = S_FindEnvironment(id);
	}
	return arc;
}

FSerializer &Serialize(FSerializer &arc, const char *key, zone_t &z, zone_t *def)
{
	return Serialize(arc, key, z.Environment, nullptr);
}

//============================================================================
//
//
//
//============================================================================

void SerializeWorld(FSerializer &arc)
{
	// fixme: This needs to ensure it reads from the correct place. Should be one once there's enough of this code converted to JSON
	arc.Array("linedefs", lines, &loadlines[0], numlines)
		.Array("sidedefs", sides, &loadsides[0], numsides)
		.Array("sectors", sectors, &loadsectors[0], numsectors)
		("subsectors", subsectors)
		("zones", Zones)
		("lineportals", linePortals)
		("sectorportals", sectorPortals);

	if (arc.isReading()) P_CollectLinkedPortals();
}


//==========================================================================
//
// ArchiveSounds
//
//==========================================================================

void P_SerializeSounds (FArchive &arc)
{
	S_SerializeSounds (arc);
	DSeqNode::SerializeSequences (arc);
	char *name = NULL;
	BYTE order;

	if (arc.IsStoring ())
	{
		order = S_GetMusic (&name);
	}
	arc << name << order;
	if (arc.IsLoading ())
	{
		if (!S_ChangeMusic (name, order))
			if (level.cdtrack == 0 || !S_ChangeCDMusic (level.cdtrack, level.cdid))
				S_ChangeMusic (level.Music, level.musicorder);
	}
	delete[] name;
}

//==========================================================================
//
// ArchivePolyobjs
//
//==========================================================================
#define ASEG_POLYOBJS	104

void P_SerializePolyobjs (FArchive &arc)
{
	int i;
	FPolyObj *po;

	if (arc.IsStoring ())
	{
		int seg = ASEG_POLYOBJS;
		arc << seg << po_NumPolyobjs;
		for(i = 0, po = polyobjs; i < po_NumPolyobjs; i++, po++)
		{
			arc << po->tag << po->Angle << po->StartSpot.pos << po->interpolation << po->bBlocked << po->bHasPortals;
			arc << po->specialdata;
  		}
	}
	else
	{
		int data;
		DAngle angle;
		DVector2 delta;

		arc << data;
		if (data != ASEG_POLYOBJS)
			I_Error ("Polyobject marker missing");

		arc << data;
		if (data != po_NumPolyobjs)
		{
			I_Error ("UnarchivePolyobjs: Bad polyobj count");
		}
		for (i = 0, po = polyobjs; i < po_NumPolyobjs; i++, po++)
		{
			arc << data;
			if (data != po->tag)
			{
				I_Error ("UnarchivePolyobjs: Invalid polyobj tag");
			}
			arc << angle << delta << po->interpolation;
			arc << po->bBlocked;
			arc << po->bHasPortals;
			if (SaveVersion >= 4548)
			{
				arc << po->specialdata;
			}

			po->RotatePolyobj (angle, true);
			delta -= po->StartSpot.pos;
			po->MovePolyobj (delta, true);
		}
	}
}

//==========================================================================
//
//
//==========================================================================

void G_SerializeLevel(FArchive &arc, bool hubLoad)
{
	int i = level.totaltime;

	unsigned tm = I_MSTime();

	Renderer->StartSerialize(arc);
	if (arc.IsLoading()) P_DestroyThinkers(hubLoad);

	arc << level.flags
		<< level.flags2
		<< level.fadeto
		<< level.found_secrets
		<< level.found_items
		<< level.killed_monsters
		<< level.gravity
		<< level.aircontrol
		<< level.teamdamage
		<< level.maptime
		<< i;

	// Hub transitions must keep the current total time
	if (!hubLoad)
		level.totaltime = i;

	arc << level.skytexture1 << level.skytexture2;
	if (arc.IsLoading())
	{
		sky1texture = level.skytexture1;
		sky2texture = level.skytexture2;
		R_InitSkyMap();
	}

	G_AirControlChanged();

	BYTE t;

	// Does this level have scrollers?
	if (arc.IsStoring())
	{
		t = level.Scrolls ? 1 : 0;
		arc << t;
	}
	else
	{
		arc << t;
		if (level.Scrolls)
		{
			delete[] level.Scrolls;
			level.Scrolls = NULL;
		}
		if (t)
		{
			level.Scrolls = new FSectorScrollValues[numsectors];
			memset(level.Scrolls, 0, sizeof(level.Scrolls)*numsectors);
		}
	}

	FBehavior::StaticSerializeModuleStates(arc);
	if (arc.IsLoading()) interpolator.ClearInterpolations();
	P_SerializeWorld(arc);
	P_SerializeThinkers(arc, hubLoad);
	P_SerializeWorldActors(arc);	// serializing actor pointers in the world data must be done after SerializeWorld has restored the entire sector state, otherwise LinkToWorld may fail.
	P_SerializePolyobjs(arc);
	P_SerializeSubsectors(arc);
	StatusBar->Serialize(arc);

	arc << level.total_monsters << level.total_items << level.total_secrets;

	// Does this level have custom translations?
	FRemapTable *trans;
	WORD w;
	if (arc.IsStoring())
	{
		for (unsigned int i = 0; i < translationtables[TRANSLATION_LevelScripted].Size(); ++i)
		{
			trans = translationtables[TRANSLATION_LevelScripted][i];
			if (trans != NULL && !trans->IsIdentity())
			{
				w = WORD(i);
				arc << w;
				trans->Serialize(arc);
			}
		}
		w = 0xffff;
		arc << w;
	}
	else
	{
		while (arc << w, w != 0xffff)
		{
			trans = translationtables[TRANSLATION_LevelScripted].GetVal(w);
			if (trans == NULL)
			{
				trans = new FRemapTable;
				translationtables[TRANSLATION_LevelScripted].SetVal(w, trans);
			}
			trans->Serialize(arc);
		}
	}

	// This must be saved, too, of course!
	FCanvasTextureInfo::Serialize(arc);
	AM_SerializeMarkers(arc);

	P_SerializePlayers(arc, hubLoad);
	P_SerializeSounds(arc);
	if (arc.IsLoading())
	{
		for (i = 0; i < numsectors; i++)
		{
			P_Recalculate3DFloors(&sectors[i]);
		}
		for (i = 0; i < MAXPLAYERS; ++i)
		{
			if (playeringame[i] && players[i].mo != NULL)
			{
				players[i].mo->SetupWeaponSlots();
			}
		}
	}
	Renderer->EndSerialize(arc);
	unsigned tt = I_MSTime();
	Printf("Serialization took %d ms\n", tt - tm);
}

