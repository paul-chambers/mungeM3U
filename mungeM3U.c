/**
   Copyright &copy; Paul Chambers, 2020.

   @ToDo Switch to UTF-8 string handling, rather than relying on ASCII backwards-compatibility
*/

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>

#include <argtable3.h>
#include <btree/btree.h>
#include "buffer.h"

#include <libhashstrings.h>
#include "keyword.h"
#include "name.h"
#include "country.h"
#include "countryCodes.h"
#include "genre.h"
#include "resolution.h"
#include "fccdata.h"
#include "nielsenDMA.h"
#include "languages.h"
#include "usstate.h"
#include "usstationdata.h"
#include "city.h"
#include "capitalization.h"

/* first pass parsing the line */

/*
 * these are common attributes that may occur in the channel and/or group name
 * so they are handled using a common structure & parsing. Attributes defined at
 * the group level are assumed to be inherited by every channel in the group,
 * unless the channel name itself has keywords to override that.
 */
typedef struct sAttr {
    const char *       name;
    tHash              hash;
    tCityIndex         city;
    tCountryIndex      country;
    tRegionIndex       region;
    tLanguage          language;
    tGenreIndex        genre;
    tAffiliateIndex    affiliate;
    tUSCallsignIndex   usStation;
    tResolutionIndex   resolution;
    bool               isVIP;
    bool               isPlus1;
    bool               isLive;
} tAttr;

typedef struct sGroup {
    struct sGroup *   next;
    tAttr             attr;
} tGroup;

typedef struct sChannel {
    struct sChannel * next;
    tAttr             attr;

    struct sEntry *   entry;
    struct sStream *  stream;
    struct sGroup *   group;
} tChannel;

typedef struct sStream {
    struct sStream *  next;
    struct sStream *  channel;
    const char     *  url;
    tHash             hash;
    tResolutionIndex  resolution;
    bool              isVIP;
} tStream;

typedef struct sEntry {
    struct sEntry *   next;
    /* first pass */
    const char *      tvg_id;
    const char *      tvg_name;
    const char *      tvg_logo;
    const char *      group_title;
    const char *      trailing;
    const char *      url;
} tEntry;

struct {
    const char *      executableName;
    FILE *            outputFile;
    int               debugLevel;

    struct {
        tEntry *      entry;
        tChannel *    channel;
        tGroup *      group;
        tStream *     stream;
    } head;
} global;

void dumpChannel( FILE * output, tChannel * channel );

#define DEBUG_REJECTION
/**
 * @brief Decide if a channel should be included in the output M3U
 *
 * This is the key function to modify to sort your needs.
 * It controls the filtering of channels imported from the original
 * M3U to produce the trimmed-down list of channels you actually
 * want to be in the output M3U.
 *
 * @param channel
 * @return true if the channel should be included
 */
bool isEnabled( tChannel * channel )
{
#if 0
    (void)channel;
    bool result = true;
#else
    bool  result = true;
    tAttr * attr = &channel->attr;

    switch ( attr->genre )
    {
        /* Filter out genres I never record */
    case kGenreAdult:
    case kGenreCivic:
    case kGenreReligious:
    case kGenreShopping:
    case kGenreSports:
        /* ...and the channels that don't make sense for a DVR */
    case kGenrePayPerView:
    case kGenre24x7:
    case kGenreVideoOnDemand:
        result = false;
#ifdef DEBUG_REJECTION
        fprintf( stderr, "   Genre ");
        dumpChannel( stderr, channel );
#endif
        break;

        /* the regional channels can bump up the channel count significantly.
         * since most methods of recording IPTV put a cap on the number of
         * channels that can be imported, removing redundant channels is worthwhile */
    case kGenreRegional:
        switch ( attr->country )
        {
        case kCountryUnitedStates:
            /* if we find a US Callsign, filter down to the SF Bay Area stations */
            if ( attr->usStation != kUSCallsignUnknown )
            {
                result = ( USStationData[ attr->usStation ].nielsenDMAIdx == kNielsenDMASFBayArea );
            }
            break;

        case kCountryCanada:
            if ( attr->city != kCityToronto && attr->city != kCityVancouver )
            {
                result = false;
            }
            break;

        case kCountryUnitedKingdom:
            /* nuke the multitude of regional channels in the UK */
            if ( attr->city != kCityLondon )
            {
                result = false;
            }
            break;

        default:
            result = false;
            break;
        }
#ifdef DEBUG_REJECTION
        if ( result == false )
        {
            fprintf( stderr, "Regional ");
            dumpChannel( stderr, channel );
        }
#endif
        break;

    case kGenreUnknown:
    default:
        break;
    }

    /* trim down to just UK and Canadian channels */
    if (result)
    {
        switch ( attr->country )
        {
        case kCountryCanada:
        case kCountryUnitedKingdom:
            break;

        default:
            result = false;
#ifdef DEBUG_REJECTION
            fprintf( stderr, " Country ");
            dumpChannel( stderr, channel );
#endif
            break;
        }
    }

    if ( result )
    {
        /* only channels that are in the english language */
        if ( attr->language != kLanguageEnglish )
        {
            result = false;
#ifdef DEBUG_REJECTION
            fprintf( stderr, "Language ");
            dumpChannel( stderr, channel );
#endif
        }
    }

    if ( result && attr->isPlus1 )
    {
        /* filter out all the +1 channels */
        result = false;
#ifdef DEBUG_REJECTION
        fprintf( stderr, "  Plus 1 ");
        dumpChannel( stderr, channel );
#endif
    }

    /* nuke all the 'live only during events' channels (typically sporting events) */
    if ( result && attr->isLive )
    {
        result = false;
#ifdef DEBUG_REJECTION
        fprintf( stderr, "    Live ");
        dumpChannel( stderr, channel );
#endif
    }
    if (result)
    {
        if ( attr->resolution == kResolutionSD )
        {
            result = false;
#ifdef DEBUG_REJECTION
            fprintf( stderr, "Resolution ");
            dumpChannel( stderr, channel );
#endif
        }
    }
#endif
    return result;
}

/* in hindsight, I probably should have used C++ instead... */

tStream * newStream( void )
{
    return (tStream *) calloc( 1, sizeof( tStream ));
}

void freeStream( tStream * stream )
{
    free( (void *)stream );
}

tChannel * newChannel(void)
{
    return (tChannel *) calloc(1,sizeof( tChannel ));
}

tChannel * freeChannel( tChannel * channel )
{
    if (channel != NULL)
    {
        tStream * stream = channel->stream;
        channel->stream = NULL;
        while ( stream != NULL )
        {
            tStream * next = stream->next;
            freeStream( stream );
            stream = next;
        }

        free( channel );
    }
    return NULL;
}

tGroup * newGroup( void )
{
    return (tGroup *) calloc( 1, sizeof( tGroup ) );
}

void freeGroup( tGroup * group )
{
    if (group != NULL)
    {
        free( (void *) group->attr.name );
        free( (void *) group );
    }
}

void dumpAttrs( FILE * output, tAttr * attr)
{
    if ( attr->name != NULL )
    {
        fprintf( output, "name: %s", attr->name );
    }
    if ( attr->genre != kGenreUnknown )
    {
        fprintf( output, ", genre: %s", lookupGenreAsString[attr->genre] );
    }
    if ( attr->usStation != kUSCallsignUnknown )
    {
        fprintf( output, ", callsign: \"%s\"",
                 USStationData[ attr->usStation ].callsign );
        fprintf( output, ", state: %s",
                 lookupUSStateAsString[ USStationData[ attr->usStation ].stateIdx ] );
        fprintf( output, ", DMA: \"%s\"",
                 lookupNielsenDMAAsString[ USStationData[ attr->usStation ].nielsenDMAIdx ] );
    }
    if ( attr->affiliate != kAffiliateUnknown )
    {
        fprintf( output, ", affiliate: %s", lookupAffiliateAsString[attr->affiliate] );
    }
    if ( attr->country != kCountryUnknown )
    {
        fprintf( output, ", country: %s", lookupFullCountryAsString[attr->country] );
        if ( attr->country == kCountryCanada && attr->city != kCityUnknown )
        {
            fprintf( output, ", city: %s", lookupCityAsString[attr->city] );
        }
    }
    if ( attr->region != kRegionUnknown )
    {
        fprintf( output, ", region: %s", lookupRegionAsString[attr->region] );
    }
    if ( attr->language != kLanguageUnknown )
    {
        fprintf( output, ", language: %s", lookupLanguageAsString[attr->language] );
    }
    if ( attr->resolution != kResolutionUnknown )
    {
        fprintf( output, ", resolution: %s", lookupResolutionAsString[ attr->resolution ] );
    }
    if ( attr->isVIP )
    {
        fprintf( output, ", VIP" );
    }
    if (attr->isPlus1)
    {
        fprintf(output,", +1");
    }
    if (attr->isLive)
    {
        fprintf(output,", Live");
    }
}

void dumpStreams( FILE * output, tStream * stream )
{
    while ( stream != NULL )
    {
        fprintf( output, "   stream: rez: %s, isvip %d, url: %s\n",
                 lookupResolutionAsString[ stream->resolution ],
                 stream->isVIP,
                 stream->url );
        stream = stream->next;
    }
}

void dumpChannel( FILE * output, tChannel * channel )
{
    fprintf( output, "  channel " );
    dumpAttrs( output, &channel->attr );

    fprintf( output, ".\n" );
}

void dumpGroup( FILE * output, tGroup * group )
{
    fprintf( output, "    group " );
    dumpAttrs( output, &group->attr );

    fprintf( output, ".\n" );
}

bool assignHash( tRecord skipTable[], tHash hash, tIndex * setting )
{
    tIndex index = findHash( skipTable, hash );
    if ( index != kIndexUnknown && *setting == kIndexUnknown )
    {
        *setting = index;
    }
    return (index != kIndexUnknown);
}

bool processAttr( tHash hash, tAttr * attr )
{
    bool swallow = false;

    if ( attr->country == kCountryUnknown && assignHash( mapCountrySearch, hash, &attr->country ))
    {
        swallow = true;
        if ( attr->region == kRegionUnknown )
        {
            attr->region = countryToRegion[ attr->country ];
        }
        if ( attr->language == kLanguageUnknown )
        {
            attr->language = countryToLanguage[ attr->country ];
        }
    }

    switch (findHash( mapNameSearch, hash ))
    {
    case kNameVIP:
        attr->isVIP = true;
        swallow = true;
        break;

    case kNameFrenchCanadian:
        attr->language = kLanguageFrench;
        swallow = true;
        break;

    case kNameIndian:
        attr->language = kLanguageHindi;
        swallow = true;
        break;

    case kNameSpanish:
        attr->language = kLanguageSpanish;
        break;

    case kNamePlus1:
        attr->isPlus1 = true;
        break;

    case kNameLive:
        attr->isLive = true;
        break;

    case kNameExYu:
        attr->country = kCountryExYu;
        break;
    }

    if ( assignHash( mapResolutionSearch, hash, &attr->resolution ) )
    {
        swallow = true;
    }

    if ( attr->country == kCountryUnitedStates || attr->country == kCountryCanada )
    {
        assignHash( mapUSCallsignSearch, hash, &attr->usStation );
        if ( attr->usStation != kUSCallsignUnknown )
        {
            attr->country   = kCountryUnitedStates;
            attr->genre     = kGenreRegional;
            attr->affiliate = USStationData[ attr->usStation ].affiliateIdx;
        }
    }
    if ( attr->genre == kGenreUnknown && assignHash( mapCitySearch, hash, &attr->city ))
    {
        attr->genre = kGenreRegional;
    }

    /* if a US Station callsign was already found, then genre has already been set to 'regional' */

    /* Allow a second genre to override 'sports', since group names of 'sports and entertainment' are common*/
    if ( attr->genre == kGenreUnknown || attr->genre == kGenreSports )
    {
        assignHash( mapGenreSearch, hash, &attr->genre );
    }

    /* This is a stronger indication of a station's language than the country, e.g. hispanic networks in the U.S. */
    if (attr->affiliate != kAffiliateUnknown)
    {
        attr->language  = affiliateToLanguage[ attr->affiliate ];
    }

    return swallow;
}


void processName( const char * name, tAttr * attr )
{
    tMappedChar   mappedC;
    const char *  p;
    char *        sp;
    char *        dp;
    int           dl;
    tHash         hash;
    char          temp[250];

    /* first extract any attributes embedded in the channel name,
     * tags like 'VIP', 'UK', 'HD', etc. See name.hash */

    p = name;

    dp = temp;
    sp = dp;
    hash = 0;

    dp[0] = '\0';
    dl = sizeof( temp ) - 1;


    bool first = true;
    do {
        char c = *p++;

        mappedC = remapChar( gNameCharMap, c );
        if ( mappedC != kNameSeparator && mappedC != '\0' )
        {
            hash = hashChar( hash, mappedC );
            if ( dl > 0 )
            {
                *dp++ = first ? toupper(c) : tolower(c);
                first = false;
                dl--;
            }
        }
        else
        {
            if ( hash != 0 )
            {
                tCapitalizationIndex capitalize = findHash( mapCapitalizationSearch, hash );
                if ( capitalize != kCapitalizationUnknown )
                {
                    const char * p = lookupCapitalizationAsString[ capitalize ];
                    memcpy( sp, p, strlen(p) );
                }
                if ( processAttr( hash, attr ) )
                {
                    /* back up to the beginning of this hash run */
                    dp = sp;
                }
                else
                {
                    /* remember the start of the next hash run */
                    if ( c != '\0' )
                    {
                        *dp++ = ' ';
                    }
                    sp = dp;
                }

                *dp = '\0';
                hash = 0;
                first = true;
            }
        }
    } while ( mappedC != '\0' );

    /* nuke the trailing space, if there is one */
    if ( dp[-1] == ' ' )
    {
        *(--dp) = '\0';
        dl++;
    }

    /* calculate the hash */
    attr->hash = hashString( temp, gNameCharMap );
    attr->name = strdup( temp );
}

/**
 * @brief
 * @param stream
 * @param name
 * @return
 */
tChannel * processChannelName( tChannel * channel, const char * name )
{
    processName( name, &channel->attr );

    tStream * stream = newStream();
    if ( stream != NULL )
    {
        /* haven't de-dup'd the channel yet, so remember any stream-specific attributes before they are lost */
        stream->isVIP      = channel->attr.isVIP;
        stream->resolution = channel->attr.resolution;
        stream->url        = channel->entry->url;

        /* Let's see if we already have a matching channel */
        tChannel ** prevChan = &global.head.channel;
        tChannel * chan;
        for ( chan = global.head.channel; chan != NULL; chan = chan->next )
        {
            /* make such the country matches, too - we removed it earlier */
            if ( channel->attr.hash    == chan->attr.hash
              && channel->attr.country == chan->attr.country )
            {
                /* channel already exists, so discard the local one */
                freeChannel( channel );
                /* and switch to the existing one */
                channel = chan;
                break;
            }
            prevChan = &chan->next;
        }
        if (chan == NULL)
        {
            /* didn't find it, so add new channel to the end of the chain */
            *prevChan = channel;
        }

        /* keep track of the best resolution stream seen  */
        if ( stream->resolution > channel->attr.resolution )
        {
            channel->attr.resolution = stream->resolution;
        }

        /* insertion sort keeps higher resolution streams first in the list */
        tStream * strm;
        tStream ** prev = &channel->stream;
        for ( strm = channel->stream; strm != NULL; strm = strm->next )
        {
            if ( stream->resolution > strm->resolution  )
            {
                stream->next = strm;
                *prev = stream;
                break;
            }
            prev = &strm->next;
        }
        if (strm == NULL)
        {
            /* resolution is not higher than any existing stream. so tack it on the end */
            *prev = stream;
        }
    }

    return channel;
}

tGroup * processGroupName( tGroup * group, const char * name )
{
    processName( name, &group->attr );

    /* Let's see if we already have a matching group */
    tGroup * prev = NULL;
    tGroup * grp = global.head.group;
    while ( grp != NULL )
    {
        if ( group->attr.hash    == grp->attr.hash
          && group->attr.country == grp->attr.country )
        {
            /* group already exists, so discard the local one */
            freeGroup( group );
            /* and switch to the existing one */
            group = grp;
            break;
        }
        prev = grp;
        grp = grp->next;
    }
    if (grp == NULL)
    {
        /* didn't find it, so add new group to the end of the chain */
        if (prev != NULL)
        {
            prev->next = group;
        } else {
            global.head.group = group;
        }
    }

    return group;
}

void inheritAttr( tGroup * group, tChannel * channel, tStream * stream )
{
    /* inherit attributes from group, as applicable */

    if (channel->attr.country == kCountryUnknown
       && group->attr.country != kCountryUnknown )
    {
        channel->attr.country = group->attr.country;
    }

    if ( channel->attr.language == kLanguageUnknown
        && group->attr.language != kLanguageUnknown )
    {
        channel->attr.language = group->attr.language;
    }

    if (channel->attr.genre == kGenreUnknown
       && group->attr.genre != kGenreUnknown)
    {
        channel->attr.genre = group->attr.genre;
    }

    if ( channel->attr.affiliate == kAffiliateUnknown
        && group->attr.affiliate != kAffiliateUnknown )
    {
        channel->attr.affiliate = group->attr.affiliate;
    }

    /* inherit resolution from group if stream resolution is unknown */
    if ( channel->attr.resolution == kResolutionUnknown )
    {
        channel->attr.resolution = group->attr.resolution;
    }
    if ( stream->resolution == kResolutionUnknown )
    {
            stream->resolution = channel->attr.resolution;
    }

    /* inherit stream VIP status from group if not already VIP */
    if ( !stream->isVIP && group->attr.isVIP )
    {
        stream->isVIP = group->attr.isVIP;
    }
}

/* parse the M3U file into a linked list of tEntry structures */
void importM3U( tBuffer * buffer )
{
    const char * str;

    tHash hash = 0;
    tHash assignmentHash = 0;
    tEntry * entry;
    const char * line;

    global.head.entry = calloc( 1, sizeof(tEntry) );
    entry = global.head.entry;

    bufferDropAnchor( buffer );

    while ( bufferGetRemaining( buffer ) > 0 )
    {
        tMappedChar w = remapChar( gKeywordCharMap, bufferGetChar( buffer ));
        switch ( w )
        {
        case kKeywordEOL: /* check hash for a known keyword */
            if ( hash != 0 )
            {
                // fprintf( stderr, "eol: 0x%016lx ", hash );
                // bufferPrintToEOL( stderr, buffer );
                switch ( findHash( mapKeywordSearch, hash ))
                {
                case kKeywordEXTM3U:
                    // fprintf(stderr, "[File Start]\n" );
                    break;

                default:
                    line = bufferGetAnchorString( buffer );
                    if ( line != NULL)
                    {
                        fprintf( stderr, "### unknown line: \'%s\'\n", line );
                        free((void *)line );
                    }
                    break;
                }
                hash = 0;
                bufferDropAnchor( buffer );
            }
            break;

        case kKeywordSeparator: /* check hash for a known keyword */
            if ( hash != 0 ) /* ignore consecutive separators */
            {
                switch ( findHash( mapKeywordSearch, hash ))
                {
                case kKeywordEXTINF:
                    // bufferPrintToEOL(stderr, buffer );
                    break;

                default:
                    line = bufferGetAnchorString( buffer );
                    if ( line != NULL)
                    {
                        fprintf( stderr, "### unknown keyword: 0x%016lx, \'%s\'\n", hash, line );
                        bufferPrintToEOL( stderr, buffer );
                        free( (void *)line );
                    }
                    break;
                }
                hash = 0;
                bufferDropAnchor( buffer );
            }
            break;

        case kKeywordAssign:
            if ( hash != 0 )
            {
                assignmentHash = hash;
                hash           = 0;
            }
            break;

        case kKeywordQuote:
            str = bufferGetQuotedString( buffer );
            if ( str != NULL )
            {
                switch ( findHash( mapKeywordSearch, assignmentHash ) )
                {
                case kKeywordID:
                    entry->tvg_id = str;
                    break;

                case kKeywordName:
                    {
                        /* Uglyness because of inconsistent formatting of '+1' channels.
                         * Some lack a separator between the channel name and the '+1'
                         * this code detects that case and inserts a space */
                        char * p = strrchr( str, '+' );
                        if ( p != NULL && p[1] == '1' && p[-1] != ' ' )
                        {
                            int len = strlen( str );
                            str = realloc( (void *)str, len + 2 );

                            char * s = p;
                            while ( *s != '\0' ) { s++; }
                            while ( *s != '+' ) { s[1] = s[0]; --s; }
                            s[1] = '+';
                            s[0] = ' ';
                        }
                        entry->tvg_name = str;
                    }
                    break;

                case kKeywordLogo:
                    entry->tvg_logo = str;
                    break;

                case kKeywordGroup:
                    entry->group_title = str;
                    break;
                }
                assignmentHash = 0;
            }
            hash = 0;
            bufferDropAnchor( buffer );

            break;

        case kKeywordComma:
            /* from here to EOL is also the name */
            str = bufferGetStringToEOL( buffer );
            entry->trailing = str;

            /* the entire next line is the URL */
            str = bufferGetStringToEOL( buffer );
            entry->url = str;

            hash = 0;
            bufferDropAnchor( buffer );

            /* get ourselves a fresh block to populate */
            entry->next = calloc( 1, sizeof(tEntry) );
            entry = entry->next;
            break;

        default:
            hash = hashChar( hash, w );
            break;
        }
    } /* getBuffRemaining */
}


void exportChannel( FILE * output, tChannel * channel )
{
#if 0
    dumpGroup( output, channel->group );
    dumpChannel( output, channel );
    dumpStreams( output, channel->stream );
#else
    char    country[20];
    char    resolution[20];

    country[0] = '\0';
    if ( channel->attr.country != kCountryUnknown )
    {
        snprintf( country, sizeof(country), " (%s)", lookupCountryAsString[ channel->attr.country ] );
    }
    resolution[0] = '\0';
    if ( channel->attr.resolution != kResolutionUnknown )
    {
        snprintf( resolution, sizeof(resolution), " [%s]", lookupResolutionAsString[ channel->attr.resolution ] );
    }

    /* fix up the group name if it's empty - happens if the original group name only contains elements that are swallowed */
    if ( channel->group->attr.name == NULL || strlen( channel->group->attr.name ) == 0 )
    {
        channel->group->attr.name = lookupFullCountryAsString[ channel->group->attr.country ];
    }

    fprintf( output, "#EXTINF:-1 tvg-id=\"%s\" tvg-name=\"%s%s%s\" tvg-logo=\"%s\" group-title=\"%s\",%s%s%s\n%s\n",
             channel->entry->tvg_id,
             channel->attr.name, country, resolution,
             channel->entry->tvg_logo,
             channel->group->attr.name,
             channel->attr.name, country, resolution,
             channel->stream->url );
#endif
}

void exportM3U( FILE * output )
{
    fprintf( output, "#EXTM3U\n" );
    for (tChannel *channel = global.head.channel; channel != NULL; channel = channel->next)
    {
        if ( isEnabled( channel ) )
        {
            exportChannel( stdout, channel );
        }
    }
}

/* process an entire M3U file */
void processM3U( tBuffer * buffer )
{
    tEntry *    entry;
    tGroup *    group;
    tChannel *  channel;
    tStream *   stream;

    importM3U( buffer );

    for ( entry = global.head.entry; entry != NULL; entry = entry->next )
    {
        group = newGroup();
        if ( group != NULL && entry->group_title != NULL )
        {
            group = processGroupName( group, entry->group_title );
        }
        channel = newChannel();
        if ( channel != NULL && entry->tvg_name != NULL )
        {
            channel->entry = entry;
            channel->group = group;
            channel = processChannelName( channel, entry->tvg_name );
            /* for unset channel attrs, inherit the group attrs */
            stream = channel->stream;
            if ( stream != NULL )
            {
                inheritAttr( group, channel, stream );
            }
        }
    }

    exportM3U( stdout );
}

int processFile( const char * path )
{
    int result = 0;

    int fd = open( path, O_RDONLY );
    struct stat fileinfo;
    if ( fstat( fd, &fileinfo ) == -1 )
    {
        result = -errno;
    }
    else
    {
        size_t length = fileinfo.st_size;
        const char * map = (const char *) mmap( NULL, length, PROT_READ, MAP_PRIVATE, fd, 0 );

        if ( map == MAP_FAILED || map == NULL )
        {
            result = -errno;
        }
        else
        {
            tBuffer * buffer = bufferNew( map, length );
            processM3U( buffer );
        }
    }
    return result;
}


const char * usage =
"Command Line Options\n"
"  -d <string>  set {destination} parameter\n"
"  -t <string>  set {template} paameter\n"
"  -x           pass each output string to the shell to execute\n"
"  --           read from stdin\n"
"  -0           stdin is null-terminated (also implies '--' option)\n"
"  -v <level>   set the level of verbosity (debug info)\n";

/* global arg_xxx structs */
static struct
{
    struct arg_lit  * help;
    struct arg_lit  * version;
    struct arg_str  * extn;
    struct arg_file * file;
    struct arg_end  * end;
} gOption;

int main( int argc, char * argv[] )
{
    int result = 0;

    global.head.group   = NULL;
    global.head.channel = NULL;
    global.head.stream  = NULL;

    global.executableName = strrchr( argv[0], '/' );
    /* If we found a slash, increment past it. If there's no slash, point at the full argv[0] */
    if ( global.executableName++ == NULL)
    { global.executableName = argv[0]; }

    global.outputFile = stdout;

    /* the global arg_xxx structs above are initialised within the argtable */
    void * argtable[] =
    {
        gOption.help    = arg_litn( NULL, "help", 0, 1,
                                    "display this help (and exit)" ),
        gOption.version = arg_litn( NULL, "version", 0, 1,
                                    "display version info (and exit)" ),
        gOption.extn    = arg_strn( "x", "extension", "<extension>", 0, 1,
                                    "set the extension to use for output files" ),
        gOption.file    = arg_filen(NULL, NULL, "<file>", 1, 999,
                                    "input files" ),

        gOption.end     = arg_end( 20 )
    };

    int nerrors = arg_parse( argc, argv, argtable );

    if ( gOption.help->count > 0 )    /* special case: '--help' takes precedence over everything else */
    {
        fprintf( stdout, "Usage: %s", global.executableName );
        arg_print_syntax( stdout, argtable, "\n" );
        fprintf( stdout, "process hash file into a header file.\n\n" );
        arg_print_glossary( stdout, argtable, "  %-25s %s\n" );
        fprintf( stdout, "\n" );

        result = 0;
    }
    else if ( gOption.version->count > 0 )   /* ditto for '--version' */
    {
        fprintf( stdout, "%s, version %s\n", global.executableName, "(to do)" );
    }
    else if ( nerrors > 0 )    /* If the parser returned any errors then display them and exit */
    {
        /* Display the error details contained in the arg_end struct.*/
        arg_print_errors( stdout, gOption.end, global.executableName );
        fprintf( stdout, "Try '%s --help' for more information.\n", global.executableName );
        result = 1;
    }
    else
    {
        global.outputFile = NULL;

#if 0
        const char * extension = ".h";
        if ( gOption.extn->count != 0 )
        {
            extension = *gOption.extn->sval;
        }
#endif
        result = 0;
        for ( int i = 0; i < gOption.file->count && result == 0; i++ )
        {
#if 0
            char output[FILENAME_MAX];
            strncpy( output, gOption.file->filename[i], sizeof( output ));


            char * p = strrchr( output, '.' );
            if ( p != NULL)
            {
                strncpy( p, extension, &output[sizeof( output ) - 1] - p );
            }

            global.outputFile = fopen( output, "w" );
            if ( global.outputFile == NULL)
            {
                fprintf( stderr, "### unable to open \'%s\' (%d: %s)\n",
                         output, errno, strerror(errno));
                result = errno;
            }
#endif
            if ( result == 0 )
            {
                result = processFile( gOption.file->filename[i] );
            }
#if 0
            fclose( global.outputFile );
#endif
        }
    }

    /* release each non-null entry in argtable[] */
    arg_freetable( argtable, sizeof( argtable ) / sizeof( argtable[0] ));

    return result;
}
