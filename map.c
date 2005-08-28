/*
 * map.c: Game involving four-colouring a map.
 */

/*
 * TODO:
 * 
 *  - clue marking
 *  - more solver brains?
 *  - better four-colouring algorithm?
 *  - pencil marks?
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>

#include "puzzles.h"

/*
 * I don't seriously anticipate wanting to change the number of
 * colours used in this game, but it doesn't cost much to use a
 * #define just in case :-)
 */
#define FOUR 4
#define THREE (FOUR-1)
#define FIVE (FOUR+1)
#define SIX (FOUR+2)

/*
 * Ghastly run-time configuration option, just for Gareth (again).
 */
static int flash_type = -1;
static float flash_length;

/*
 * Difficulty levels. I do some macro ickery here to ensure that my
 * enum and the various forms of my name list always match up.
 */
#define DIFFLIST(A) \
    A(EASY,Easy,e) \
    A(NORMAL,Normal,n) \
    A(RECURSE,Unreasonable,u)
#define ENUM(upper,title,lower) DIFF_ ## upper,
#define TITLE(upper,title,lower) #title,
#define ENCODE(upper,title,lower) #lower
#define CONFIG(upper,title,lower) ":" #title
enum { DIFFLIST(ENUM) DIFFCOUNT };
static char const *const map_diffnames[] = { DIFFLIST(TITLE) };
static char const map_diffchars[] = DIFFLIST(ENCODE);
#define DIFFCONFIG DIFFLIST(CONFIG)

enum { TE, BE, LE, RE };               /* top/bottom/left/right edges */

enum {
    COL_BACKGROUND,
    COL_GRID,
    COL_0, COL_1, COL_2, COL_3,
    COL_ERROR, COL_ERRTEXT,
    NCOLOURS
};

struct game_params {
    int w, h, n, diff;
};

struct map {
    int refcount;
    int *map;
    int *graph;
    int n;
    int ngraph;
    int *immutable;
    int *edgex, *edgey;		       /* positions of a point on each edge */
};

struct game_state {
    game_params p;
    struct map *map;
    int *colouring;
    int completed, cheated;
};

static game_params *default_params(void)
{
    game_params *ret = snew(game_params);

    ret->w = 20;
    ret->h = 15;
    ret->n = 30;
    ret->diff = DIFF_NORMAL;

    return ret;
}

static const struct game_params map_presets[] = {
    {20, 15, 30, DIFF_EASY},
    {20, 15, 30, DIFF_NORMAL},
    {30, 25, 75, DIFF_NORMAL},
};

static int game_fetch_preset(int i, char **name, game_params **params)
{
    game_params *ret;
    char str[80];

    if (i < 0 || i >= lenof(map_presets))
        return FALSE;

    ret = snew(game_params);
    *ret = map_presets[i];

    sprintf(str, "%dx%d, %d regions, %s", ret->w, ret->h, ret->n,
	    map_diffnames[ret->diff]);

    *name = dupstr(str);
    *params = ret;
    return TRUE;
}

static void free_params(game_params *params)
{
    sfree(params);
}

static game_params *dup_params(game_params *params)
{
    game_params *ret = snew(game_params);
    *ret = *params;		       /* structure copy */
    return ret;
}

static void decode_params(game_params *params, char const *string)
{
    char const *p = string;

    params->w = atoi(p);
    while (*p && isdigit((unsigned char)*p)) p++;
    if (*p == 'x') {
        p++;
        params->h = atoi(p);
        while (*p && isdigit((unsigned char)*p)) p++;
    } else {
        params->h = params->w;
    }
    if (*p == 'n') {
	p++;
	params->n = atoi(p);
	while (*p && (*p == '.' || isdigit((unsigned char)*p))) p++;
    } else {
	params->n = params->w * params->h / 8;
    }
    if (*p == 'd') {
	int i;
	p++;
	for (i = 0; i < DIFFCOUNT; i++)
	    if (*p == map_diffchars[i])
		params->diff = i;
	if (*p) p++;
    }
}

static char *encode_params(game_params *params, int full)
{
    char ret[400];

    sprintf(ret, "%dx%dn%d", params->w, params->h, params->n);
    if (full)
	sprintf(ret + strlen(ret), "d%c", map_diffchars[params->diff]);

    return dupstr(ret);
}

static config_item *game_configure(game_params *params)
{
    config_item *ret;
    char buf[80];

    ret = snewn(5, config_item);

    ret[0].name = "Width";
    ret[0].type = C_STRING;
    sprintf(buf, "%d", params->w);
    ret[0].sval = dupstr(buf);
    ret[0].ival = 0;

    ret[1].name = "Height";
    ret[1].type = C_STRING;
    sprintf(buf, "%d", params->h);
    ret[1].sval = dupstr(buf);
    ret[1].ival = 0;

    ret[2].name = "Regions";
    ret[2].type = C_STRING;
    sprintf(buf, "%d", params->n);
    ret[2].sval = dupstr(buf);
    ret[2].ival = 0;

    ret[3].name = "Difficulty";
    ret[3].type = C_CHOICES;
    ret[3].sval = DIFFCONFIG;
    ret[3].ival = params->diff;

    ret[4].name = NULL;
    ret[4].type = C_END;
    ret[4].sval = NULL;
    ret[4].ival = 0;

    return ret;
}

static game_params *custom_params(config_item *cfg)
{
    game_params *ret = snew(game_params);

    ret->w = atoi(cfg[0].sval);
    ret->h = atoi(cfg[1].sval);
    ret->n = atoi(cfg[2].sval);
    ret->diff = cfg[3].ival;

    return ret;
}

static char *validate_params(game_params *params, int full)
{
    if (params->w < 2 || params->h < 2)
	return "Width and height must be at least two";
    if (params->n < 5)
	return "Must have at least five regions";
    if (params->n > params->w * params->h)
	return "Too many regions to fit in grid";
    return NULL;
}

/* ----------------------------------------------------------------------
 * Cumulative frequency table functions.
 */

/*
 * Initialise a cumulative frequency table. (Hardly worth writing
 * this function; all it does is to initialise everything in the
 * array to zero.)
 */
static void cf_init(int *table, int n)
{
    int i;

    for (i = 0; i < n; i++)
	table[i] = 0;
}

/*
 * Increment the count of symbol `sym' by `count'.
 */
static void cf_add(int *table, int n, int sym, int count)
{
    int bit;

    bit = 1;
    while (sym != 0) {
	if (sym & bit) {
	    table[sym] += count;
	    sym &= ~bit;
	}
	bit <<= 1;
    }

    table[0] += count;
}

/*
 * Cumulative frequency lookup: return the total count of symbols
 * with value less than `sym'.
 */
static int cf_clookup(int *table, int n, int sym)
{
    int bit, index, limit, count;

    if (sym == 0)
	return 0;

    assert(0 < sym && sym <= n);

    count = table[0];		       /* start with the whole table size */

    bit = 1;
    while (bit < n)
	bit <<= 1;

    limit = n;

    while (bit > 0) {
	/*
	 * Find the least number with its lowest set bit in this
	 * position which is greater than or equal to sym.
	 */
	index = ((sym + bit - 1) &~ (bit * 2 - 1)) + bit;

	if (index < limit) {
	    count -= table[index];
	    limit = index;
	}

	bit >>= 1;
    }

    return count;
}

/*
 * Single frequency lookup: return the count of symbol `sym'.
 */
static int cf_slookup(int *table, int n, int sym)
{
    int count, bit;

    assert(0 <= sym && sym < n);

    count = table[sym];

    for (bit = 1; sym+bit < n && !(sym & bit); bit <<= 1)
	count -= table[sym+bit];

    return count;
}

/*
 * Return the largest symbol index such that the cumulative
 * frequency up to that symbol is less than _or equal to_ count.
 */
static int cf_whichsym(int *table, int n, int count) {
    int bit, sym, top;

    assert(count >= 0 && count < table[0]);

    bit = 1;
    while (bit < n)
	bit <<= 1;

    sym = 0;
    top = table[0];

    while (bit > 0) {
	if (sym+bit < n) {
	    if (count >= top - table[sym+bit])
		sym += bit;
	    else
		top -= table[sym+bit];
	}

	bit >>= 1;
    }

    return sym;
}

/* ----------------------------------------------------------------------
 * Map generation.
 * 
 * FIXME: this isn't entirely optimal at present, because it
 * inherently prioritises growing the largest region since there
 * are more squares adjacent to it. This acts as a destabilising
 * influence leading to a few large regions and mostly small ones.
 * It might be better to do it some other way.
 */

#define WEIGHT_INCREASED 2             /* for increased perimeter */
#define WEIGHT_DECREASED 4             /* for decreased perimeter */
#define WEIGHT_UNCHANGED 3             /* for unchanged perimeter */

/*
 * Look at a square and decide which colours can be extended into
 * it.
 * 
 * If called with index < 0, it adds together one of
 * WEIGHT_INCREASED, WEIGHT_DECREASED or WEIGHT_UNCHANGED for each
 * colour that has a valid extension (according to the effect that
 * it would have on the perimeter of the region being extended) and
 * returns the overall total.
 * 
 * If called with index >= 0, it returns one of the possible
 * colours depending on the value of index, in such a way that the
 * number of possible inputs which would give rise to a given
 * return value correspond to the weight of that value.
 */
static int extend_options(int w, int h, int n, int *map,
                          int x, int y, int index)
{
    int c, i, dx, dy;
    int col[8];
    int total = 0;

    if (map[y*w+x] >= 0) {
        assert(index < 0);
        return 0;                      /* can't do this square at all */
    }

    /*
     * Fetch the eight neighbours of this square, in order around
     * the square.
     */
    for (dy = -1; dy <= +1; dy++)
        for (dx = -1; dx <= +1; dx++) {
            int index = (dy < 0 ? 6-dx : dy > 0 ? 2+dx : 2*(1+dx));
            if (x+dx >= 0 && x+dx < w && y+dy >= 0 && y+dy < h)
                col[index] = map[(y+dy)*w+(x+dx)];
            else
                col[index] = -1;
        }

    /*
     * Iterate over each colour that might be feasible.
     * 
     * FIXME: this routine currently has O(n) running time. We
     * could turn it into O(FOUR) by only bothering to iterate over
     * the colours mentioned in the four neighbouring squares.
     */

    for (c = 0; c < n; c++) {
        int count, neighbours, runs;

        /*
         * One of the even indices of col (representing the
         * orthogonal neighbours of this square) must be equal to
         * c, or else this square is not adjacent to region c and
         * obviously cannot become an extension of it at this time.
         */
        neighbours = 0;
        for (i = 0; i < 8; i += 2)
            if (col[i] == c)
                neighbours++;
        if (!neighbours)
            continue;

        /*
         * Now we know this square is adjacent to region c. The
         * next question is, would extending it cause the region to
         * become non-simply-connected? If so, we mustn't do it.
         * 
         * We determine this by looking around col to see if we can
         * find more than one separate run of colour c.
         */
        runs = 0;
        for (i = 0; i < 8; i++)
            if (col[i] == c && col[(i+1) & 7] != c)
                runs++;
        if (runs > 1)
            continue;

        assert(runs == 1);

        /*
         * This square is a possibility. Determine its effect on
         * the region's perimeter (computed from the number of
         * orthogonal neighbours - 1 means a perimeter increase, 3
         * a decrease, 2 no change; 4 is impossible because the
         * region would already not be simply connected) and we're
         * done.
         */
        assert(neighbours > 0 && neighbours < 4);
        count = (neighbours == 1 ? WEIGHT_INCREASED :
                 neighbours == 2 ? WEIGHT_UNCHANGED : WEIGHT_DECREASED);

        total += count;
        if (index >= 0 && index < count)
            return c;
        else
            index -= count;
    }

    assert(index < 0);

    return total;
}

static void genmap(int w, int h, int n, int *map, random_state *rs)
{
    int wh = w*h;
    int x, y, i, k;
    int *tmp;

    assert(n <= wh);
    tmp = snewn(wh, int);

    /*
     * Clear the map, and set up `tmp' as a list of grid indices.
     */
    for (i = 0; i < wh; i++) {
        map[i] = -1;
        tmp[i] = i;
    }

    /*
     * Place the region seeds by selecting n members from `tmp'.
     */
    k = wh;
    for (i = 0; i < n; i++) {
        int j = random_upto(rs, k);
        map[tmp[j]] = i;
        tmp[j] = tmp[--k];
    }

    /*
     * Re-initialise `tmp' as a cumulative frequency table. This
     * will store the number of possible region colours we can
     * extend into each square.
     */
    cf_init(tmp, wh);

    /*
     * Go through the grid and set up the initial cumulative
     * frequencies.
     */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            cf_add(tmp, wh, y*w+x,
                   extend_options(w, h, n, map, x, y, -1));

    /*
     * Now repeatedly choose a square we can extend a region into,
     * and do so.
     */
    while (tmp[0] > 0) {
        int k = random_upto(rs, tmp[0]);
        int sq;
        int colour;
        int xx, yy;

        sq = cf_whichsym(tmp, wh, k);
        k -= cf_clookup(tmp, wh, sq);
        x = sq % w;
        y = sq / w;
        colour = extend_options(w, h, n, map, x, y, k);

        map[sq] = colour;

        /*
         * Re-scan the nine cells around the one we've just
         * modified.
         */
        for (yy = max(y-1, 0); yy < min(y+2, h); yy++)
            for (xx = max(x-1, 0); xx < min(x+2, w); xx++) {
                cf_add(tmp, wh, yy*w+xx,
                       -cf_slookup(tmp, wh, yy*w+xx) +
                       extend_options(w, h, n, map, xx, yy, -1));
            }
    }

    /*
     * Finally, go through and normalise the region labels into
     * order, meaning that indistinguishable maps are actually
     * identical.
     */
    for (i = 0; i < n; i++)
        tmp[i] = -1;
    k = 0;
    for (i = 0; i < wh; i++) {
        assert(map[i] >= 0);
        if (tmp[map[i]] < 0)
            tmp[map[i]] = k++;
        map[i] = tmp[map[i]];
    }

    sfree(tmp);
}

/* ----------------------------------------------------------------------
 * Functions to handle graphs.
 */

/*
 * Having got a map in a square grid, convert it into a graph
 * representation.
 */
static int gengraph(int w, int h, int n, int *map, int *graph)
{
    int i, j, x, y;

    /*
     * Start by setting the graph up as an adjacency matrix. We'll
     * turn it into a list later.
     */
    for (i = 0; i < n*n; i++)
	graph[i] = 0;

    /*
     * Iterate over the map looking for all adjacencies.
     */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
	    int v, vx, vy;
	    v = map[y*w+x];
	    if (x+1 < w && (vx = map[y*w+(x+1)]) != v)
		graph[v*n+vx] = graph[vx*n+v] = 1;
	    if (y+1 < h && (vy = map[(y+1)*w+x]) != v)
		graph[v*n+vy] = graph[vy*n+v] = 1;
	}

    /*
     * Turn the matrix into a list.
     */
    for (i = j = 0; i < n*n; i++)
	if (graph[i])
	    graph[j++] = i;

    return j;
}

static int graph_edge_index(int *graph, int n, int ngraph, int i, int j)
{
    int v = i*n+j;
    int top, bot, mid;

    bot = -1;
    top = ngraph;
    while (top - bot > 1) {
	mid = (top + bot) / 2;
	if (graph[mid] == v)
	    return mid;
	else if (graph[mid] < v)
	    bot = mid;
	else
	    top = mid;
    }
    return -1;
}

#define graph_adjacent(graph, n, ngraph, i, j) \
    (graph_edge_index((graph), (n), (ngraph), (i), (j)) >= 0)

static int graph_vertex_start(int *graph, int n, int ngraph, int i)
{
    int v = i*n;
    int top, bot, mid;

    bot = -1;
    top = ngraph;
    while (top - bot > 1) {
	mid = (top + bot) / 2;
	if (graph[mid] < v)
	    bot = mid;
	else
	    top = mid;
    }
    return top;
}

/* ----------------------------------------------------------------------
 * Generate a four-colouring of a graph.
 *
 * FIXME: it would be nice if we could convert this recursion into
 * pseudo-recursion using some sort of explicit stack array, for
 * the sake of the Palm port and its limited stack.
 */

static int fourcolour_recurse(int *graph, int n, int ngraph,
			      int *colouring, int *scratch, random_state *rs)
{
    int nfree, nvert, start, i, j, k, c, ci;
    int cs[FOUR];

    /*
     * Find the smallest number of free colours in any uncoloured
     * vertex, and count the number of such vertices.
     */

    nfree = FIVE;		       /* start off bigger than FOUR! */
    nvert = 0;
    for (i = 0; i < n; i++)
	if (colouring[i] < 0 && scratch[i*FIVE+FOUR] <= nfree) {
	    if (nfree > scratch[i*FIVE+FOUR]) {
		nfree = scratch[i*FIVE+FOUR];
		nvert = 0;
	    }
	    nvert++;
	}

    /*
     * If there aren't any uncoloured vertices at all, we're done.
     */
    if (nvert == 0)
	return TRUE;		       /* we've got a colouring! */

    /*
     * Pick a random vertex in that set.
     */
    j = random_upto(rs, nvert);
    for (i = 0; i < n; i++)
	if (colouring[i] < 0 && scratch[i*FIVE+FOUR] == nfree)
	    if (j-- == 0)
		break;
    assert(i < n);
    start = graph_vertex_start(graph, n, ngraph, i);

    /*
     * Loop over the possible colours for i, and recurse for each
     * one.
     */
    ci = 0;
    for (c = 0; c < FOUR; c++)
	if (scratch[i*FIVE+c] == 0)
	    cs[ci++] = c;
    shuffle(cs, ci, sizeof(*cs), rs);

    while (ci-- > 0) {
	c = cs[ci];

	/*
	 * Fill in this colour.
	 */
	colouring[i] = c;

	/*
	 * Update the scratch space to reflect a new neighbour
	 * of this colour for each neighbour of vertex i.
	 */
	for (j = start; j < ngraph && graph[j] < n*(i+1); j++) {
	    k = graph[j] - i*n;
	    if (scratch[k*FIVE+c] == 0)
		scratch[k*FIVE+FOUR]--;
	    scratch[k*FIVE+c]++;
	}

	/*
	 * Recurse.
	 */
	if (fourcolour_recurse(graph, n, ngraph, colouring, scratch, rs))
	    return TRUE;	       /* got one! */

	/*
	 * If that didn't work, clean up and try again with a
	 * different colour.
	 */
	for (j = start; j < ngraph && graph[j] < n*(i+1); j++) {
	    k = graph[j] - i*n;
	    scratch[k*FIVE+c]--;
	    if (scratch[k*FIVE+c] == 0)
		scratch[k*FIVE+FOUR]++;
	}
	colouring[i] = -1;
    }

    /*
     * If we reach here, we were unable to find a colouring at all.
     * (This doesn't necessarily mean the Four Colour Theorem is
     * violated; it might just mean we've gone down a dead end and
     * need to back up and look somewhere else. It's only an FCT
     * violation if we get all the way back up to the top level and
     * still fail.)
     */
    return FALSE;
}

static void fourcolour(int *graph, int n, int ngraph, int *colouring,
		       random_state *rs)
{
    int *scratch;
    int i;

    /*
     * For each vertex and each colour, we store the number of
     * neighbours that have that colour. Also, we store the number
     * of free colours for the vertex.
     */
    scratch = snewn(n * FIVE, int);
    for (i = 0; i < n * FIVE; i++)
	scratch[i] = (i % FIVE == FOUR ? FOUR : 0);

    /*
     * Clear the colouring to start with.
     */
    for (i = 0; i < n; i++)
	colouring[i] = -1;

    i = fourcolour_recurse(graph, n, ngraph, colouring, scratch, rs);
    assert(i);			       /* by the Four Colour Theorem :-) */

    sfree(scratch);
}

/* ----------------------------------------------------------------------
 * Non-recursive solver.
 */

struct solver_scratch {
    unsigned char *possible;	       /* bitmap of colours for each region */
    int *graph;
    int n;
    int ngraph;
    int depth;
};

static struct solver_scratch *new_scratch(int *graph, int n, int ngraph)
{
    struct solver_scratch *sc;

    sc = snew(struct solver_scratch);
    sc->graph = graph;
    sc->n = n;
    sc->ngraph = ngraph;
    sc->possible = snewn(n, unsigned char);
    sc->depth = 0;

    return sc;
}

static void free_scratch(struct solver_scratch *sc)
{
    sfree(sc->possible);
    sfree(sc);
}

static int place_colour(struct solver_scratch *sc,
			int *colouring, int index, int colour)
{
    int *graph = sc->graph, n = sc->n, ngraph = sc->ngraph;
    int j, k;

    if (!(sc->possible[index] & (1 << colour)))
	return FALSE;		       /* can't do it */

    sc->possible[index] = 1 << colour;
    colouring[index] = colour;

    /*
     * Rule out this colour from all the region's neighbours.
     */
    for (j = graph_vertex_start(graph, n, ngraph, index);
	 j < ngraph && graph[j] < n*(index+1); j++) {
	k = graph[j] - index*n;
	sc->possible[k] &= ~(1 << colour);
    }

    return TRUE;
}

/*
 * Returns 0 for impossible, 1 for success, 2 for failure to
 * converge (i.e. puzzle is either ambiguous or just too
 * difficult).
 */
static int map_solver(struct solver_scratch *sc,
		      int *graph, int n, int ngraph, int *colouring,
                      int difficulty)
{
    int i;

    /*
     * Initialise scratch space.
     */
    for (i = 0; i < n; i++)
	sc->possible[i] = (1 << FOUR) - 1;

    /*
     * Place clues.
     */
    for (i = 0; i < n; i++)
	if (colouring[i] >= 0) {
	    if (!place_colour(sc, colouring, i, colouring[i]))
		return 0;	       /* the clues aren't even consistent! */
	}

    /*
     * Now repeatedly loop until we find nothing further to do.
     */
    while (1) {
	int done_something = FALSE;

        if (difficulty < DIFF_EASY)
            break;                     /* can't do anything at all! */

	/*
	 * Simplest possible deduction: find a region with only one
	 * possible colour.
	 */
	for (i = 0; i < n; i++) if (colouring[i] < 0) {
	    int p = sc->possible[i];

	    if (p == 0)
		return 0;	       /* puzzle is inconsistent */

	    if ((p & (p-1)) == 0) {    /* p is a power of two */
		int c;
		for (c = 0; c < FOUR; c++)
		    if (p == (1 << c))
			break;
		assert(c < FOUR);
		if (!place_colour(sc, colouring, i, c))
		    return 0;	       /* found puzzle to be inconsistent */
		done_something = TRUE;
	    }
	}

        if (done_something)
            continue;

        if (difficulty < DIFF_NORMAL)
            break;                     /* can't do anything harder */

        /*
         * Failing that, go up one level. Look for pairs of regions
         * which (a) both have the same pair of possible colours,
         * (b) are adjacent to one another, (c) are adjacent to the
         * same region, and (d) that region still thinks it has one
         * or both of those possible colours.
         * 
         * Simplest way to do this is by going through the graph
         * edge by edge, so that we start with property (b) and
         * then look for (a) and finally (c) and (d).
         */
        for (i = 0; i < ngraph; i++) {
            int j1 = graph[i] / n, j2 = graph[i] % n;
            int j, k, v, v2;

            if (j1 > j2)
                continue;              /* done it already, other way round */

            if (colouring[j1] >= 0 || colouring[j2] >= 0)
                continue;              /* they're not undecided */

            if (sc->possible[j1] != sc->possible[j2])
                continue;              /* they don't have the same possibles */

            v = sc->possible[j1];
            /*
             * See if v contains exactly two set bits.
             */
            v2 = v & -v;           /* find lowest set bit */
            v2 = v & ~v2;          /* clear it */
            if (v2 == 0 || (v2 & (v2-1)) != 0)   /* not power of 2 */
                continue;

            /*
             * We've found regions j1 and j2 satisfying properties
             * (a) and (b): they have two possible colours between
             * them, and since they're adjacent to one another they
             * must use _both_ those colours between them.
             * Therefore, if they are both adjacent to any other
             * region then that region cannot be either colour.
             * 
             * Go through the neighbours of j1 and see if any are
             * shared with j2.
             */
            for (j = graph_vertex_start(graph, n, ngraph, j1);
                 j < ngraph && graph[j] < n*(j1+1); j++) {
                k = graph[j] - j1*n;
                if (graph_adjacent(graph, n, ngraph, k, j2) &&
                    (sc->possible[k] & v)) {
                    sc->possible[k] &= ~v;
                    done_something = TRUE;
                }
            }
        }

	if (!done_something)
	    break;
    }

    /*
     * See if we've got a complete solution, and return if so.
     */
    for (i = 0; i < n; i++)
	if (colouring[i] < 0)
            break;
    if (i == n)
        return 1;                      /* success! */

    /*
     * If recursion is not permissible, we now give up.
     */
    if (difficulty < DIFF_RECURSE)
        return 2;                      /* unable to complete */

    /*
     * Now we've got to do something recursive. So first hunt for a
     * currently-most-constrained region.
     */
    {
        int best, bestc;
        struct solver_scratch *rsc;
        int *subcolouring, *origcolouring;
        int ret, subret;
        int we_already_got_one;

        best = -1;
        bestc = FIVE;

        for (i = 0; i < n; i++) if (colouring[i] < 0) {
            int p = sc->possible[i];
            enum { compile_time_assertion = 1 / (FOUR <= 4) };
            int c;

            /* Count the set bits. */
            c = (p & 5) + ((p >> 1) & 5);
            c = (c & 3) + ((c >> 2) & 3);
            assert(c > 1);             /* or colouring[i] would be >= 0 */

            if (c < bestc) {
                best = i;
                bestc = c;
            }
        }

        assert(best >= 0);             /* or we'd be solved already */

        /*
         * Now iterate over the possible colours for this region.
         */
        rsc = new_scratch(graph, n, ngraph);
        rsc->depth = sc->depth + 1;
        origcolouring = snewn(n, int);
        memcpy(origcolouring, colouring, n * sizeof(int));
        subcolouring = snewn(n, int);
        we_already_got_one = FALSE;
        ret = 0;

        for (i = 0; i < FOUR; i++) {
            if (!(sc->possible[best] & (1 << i)))
                continue;

            memcpy(subcolouring, origcolouring, n * sizeof(int));
            subcolouring[best] = i;
            subret = map_solver(rsc, graph, n, ngraph,
                                subcolouring, difficulty);

            /*
             * If this possibility turned up more than one valid
             * solution, or if it turned up one and we already had
             * one, we're definitely ambiguous.
             */
            if (subret == 2 || (subret == 1 && we_already_got_one)) {
                ret = 2;
                break;
            }

            /*
             * If this possibility turned up one valid solution and
             * it's the first we've seen, copy it into the output.
             */
            if (subret == 1) {
                memcpy(colouring, subcolouring, n * sizeof(int));
                we_already_got_one = TRUE;
                ret = 1;
            }

            /*
             * Otherwise, this guess led to a contradiction, so we
             * do nothing.
             */
        }

        sfree(subcolouring);
        free_scratch(rsc);

        return ret;
    }
}

/* ----------------------------------------------------------------------
 * Game generation main function.
 */

static char *new_game_desc(game_params *params, random_state *rs,
			   char **aux, int interactive)
{
    struct solver_scratch *sc = NULL;
    int *map, *graph, ngraph, *colouring, *colouring2, *regions;
    int i, j, w, h, n, solveret, cfreq[FOUR];
    int wh;
    int mindiff, tries;
#ifdef GENERATION_DIAGNOSTICS
    int x, y;
#endif
    char *ret, buf[80];
    int retlen, retsize;

    w = params->w;
    h = params->h;
    n = params->n;
    wh = w*h;

    *aux = NULL;

    map = snewn(wh, int);
    graph = snewn(n*n, int);
    colouring = snewn(n, int);
    colouring2 = snewn(n, int);
    regions = snewn(n, int);

    /*
     * This is the minimum difficulty below which we'll completely
     * reject a map design. Normally we set this to one below the
     * requested difficulty, ensuring that we have the right
     * result. However, for particularly dense maps or maps with
     * particularly few regions it might not be possible to get the
     * desired difficulty, so we will eventually drop this down to
     * -1 to indicate that any old map will do.
     */
    mindiff = params->diff;
    tries = 50;

    while (1) {

        /*
         * Create the map.
         */
        genmap(w, h, n, map, rs);

#ifdef GENERATION_DIAGNOSTICS
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int v = map[y*w+x];
                if (v >= 62)
                    putchar('!');
                else if (v >= 36)
                    putchar('a' + v-36);
                else if (v >= 10)
                    putchar('A' + v-10);
                else
                    putchar('0' + v);
            }
            putchar('\n');
        }
#endif

        /*
         * Convert the map into a graph.
         */
        ngraph = gengraph(w, h, n, map, graph);

#ifdef GENERATION_DIAGNOSTICS
        for (i = 0; i < ngraph; i++)
            printf("%d-%d\n", graph[i]/n, graph[i]%n);
#endif

        /*
         * Colour the map.
         */
        fourcolour(graph, n, ngraph, colouring, rs);

#ifdef GENERATION_DIAGNOSTICS
        for (i = 0; i < n; i++)
            printf("%d: %d\n", i, colouring[i]);

        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                int v = colouring[map[y*w+x]];
                if (v >= 36)
                    putchar('a' + v-36);
                else if (v >= 10)
                    putchar('A' + v-10);
                else
                    putchar('0' + v);
            }
            putchar('\n');
        }
#endif

        /*
         * Encode the solution as an aux string.
         */
        if (*aux)                      /* in case we've come round again */
            sfree(*aux);
        retlen = retsize = 0;
        ret = NULL;
        for (i = 0; i < n; i++) {
            int len;

            if (colouring[i] < 0)
                continue;

            len = sprintf(buf, "%s%d:%d", i ? ";" : "S;", colouring[i], i);
            if (retlen + len >= retsize) {
                retsize = retlen + len + 256;
                ret = sresize(ret, retsize, char);
            }
            strcpy(ret + retlen, buf);
            retlen += len;
        }
        *aux = ret;

        /*
         * Remove the region colours one by one, keeping
         * solubility. Also ensure that there always remains at
         * least one region of every colour, so that the user can
         * drag from somewhere.
         */
        for (i = 0; i < FOUR; i++)
            cfreq[i] = 0;
        for (i = 0; i < n; i++) {
            regions[i] = i;
            cfreq[colouring[i]]++;
        }
        for (i = 0; i < FOUR; i++)
            if (cfreq[i] == 0)
                continue;

        shuffle(regions, n, sizeof(*regions), rs);

        if (sc) free_scratch(sc);
        sc = new_scratch(graph, n, ngraph);

        for (i = 0; i < n; i++) {
            j = regions[i];

            if (cfreq[colouring[j]] == 1)
                continue;              /* can't remove last region of colour */

            memcpy(colouring2, colouring, n*sizeof(int));
            colouring2[j] = -1;
            solveret = map_solver(sc, graph, n, ngraph, colouring2,
				  params->diff);
            assert(solveret >= 0);	       /* mustn't be impossible! */
            if (solveret == 1) {
                cfreq[colouring[j]]--;
                colouring[j] = -1;
            }
        }

#ifdef GENERATION_DIAGNOSTICS
        for (i = 0; i < n; i++)
            if (colouring[i] >= 0) {
                if (i >= 62)
                    putchar('!');
                else if (i >= 36)
                    putchar('a' + i-36);
                else if (i >= 10)
                    putchar('A' + i-10);
                else
                    putchar('0' + i);
                printf(": %d\n", colouring[i]);
            }
#endif

        /*
         * Finally, check that the puzzle is _at least_ as hard as
         * required, and indeed that it isn't already solved.
         * (Calling map_solver with negative difficulty ensures the
         * latter - if a solver which _does nothing_ can't solve
         * it, it's too easy!)
         */
        memcpy(colouring2, colouring, n*sizeof(int));
        if (map_solver(sc, graph, n, ngraph, colouring2,
                       mindiff - 1) == 1) {
	    /*
	     * Drop minimum difficulty if necessary.
	     */
	    if (mindiff > 0 && (n < 9 || n > 3*wh/2)) {
		if (tries-- <= 0)
		    mindiff = 0;       /* give up and go for Easy */
	    }
            continue;
	}

        break;
    }

    /*
     * Encode as a game ID. We do this by:
     * 
     * 	- first going along the horizontal edges row by row, and
     * 	  then the vertical edges column by column
     * 	- encoding the lengths of runs of edges and runs of
     * 	  non-edges
     * 	- the decoder will reconstitute the region boundaries from
     * 	  this and automatically number them the same way we did
     * 	- then we encode the initial region colours in a Slant-like
     * 	  fashion (digits 0-3 interspersed with letters giving
     * 	  lengths of runs of empty spaces).
     */
    retlen = retsize = 0;
    ret = NULL;

    {
	int run, pv;

	/*
	 * Start with a notional non-edge, so that there'll be an
	 * explicit `a' to distinguish the case where we start with
	 * an edge.
	 */
	run = 1;
	pv = 0;

	for (i = 0; i < w*(h-1) + (w-1)*h; i++) {
	    int x, y, dx, dy, v;

	    if (i < w*(h-1)) {
		/* Horizontal edge. */
		y = i / w;
		x = i % w;
		dx = 0;
		dy = 1;
	    } else {
		/* Vertical edge. */
		x = (i - w*(h-1)) / h;
		y = (i - w*(h-1)) % h;
		dx = 1;
		dy = 0;
	    }

	    if (retlen + 10 >= retsize) {
		retsize = retlen + 256;
		ret = sresize(ret, retsize, char);
	    }

	    v = (map[y*w+x] != map[(y+dy)*w+(x+dx)]);

	    if (pv != v) {
		ret[retlen++] = 'a'-1 + run;
		run = 1;
		pv = v;
	    } else {
		/*
		 * 'z' is a special case in this encoding. Rather
		 * than meaning a run of 26 and a state switch, it
		 * means a run of 25 and _no_ state switch, because
		 * otherwise there'd be no way to encode runs of
		 * more than 26.
		 */
		if (run == 25) {
		    ret[retlen++] = 'z';
		    run = 0;
		}
		run++;
	    }
	}

	ret[retlen++] = 'a'-1 + run;
	ret[retlen++] = ',';

	run = 0;
	for (i = 0; i < n; i++) {
	    if (retlen + 10 >= retsize) {
		retsize = retlen + 256;
		ret = sresize(ret, retsize, char);
	    }

	    if (colouring[i] < 0) {
		/*
		 * In _this_ encoding, 'z' is a run of 26, since
		 * there's no implicit state switch after each run.
		 * Confusingly different, but more compact.
		 */
		if (run == 26) {
		    ret[retlen++] = 'z';
		    run = 0;
		}
		run++;
	    } else {
		if (run > 0)
		    ret[retlen++] = 'a'-1 + run;
		ret[retlen++] = '0' + colouring[i];
		run = 0;
	    }
	}
	if (run > 0)
	    ret[retlen++] = 'a'-1 + run;
	ret[retlen] = '\0';

	assert(retlen < retsize);
    }

    free_scratch(sc);
    sfree(regions);
    sfree(colouring2);
    sfree(colouring);
    sfree(graph);
    sfree(map);

    return ret;
}

static char *parse_edge_list(game_params *params, char **desc, int *map)
{
    int w = params->w, h = params->h, wh = w*h, n = params->n;
    int i, k, pos, state;
    char *p = *desc;

    for (i = 0; i < wh; i++)
	map[wh+i] = i;

    pos = -1;
    state = 0;

    /*
     * Parse the game description to get the list of edges, and
     * build up a disjoint set forest as we go (by identifying
     * pairs of squares whenever the edge list shows a non-edge).
     */
    while (*p && *p != ',') {
	if (*p < 'a' || *p > 'z')
	    return "Unexpected character in edge list";
	if (*p == 'z')
	    k = 25;
	else
	    k = *p - 'a' + 1;
	while (k-- > 0) {
	    int x, y, dx, dy;

	    if (pos < 0) {
		pos++;
		continue;
	    } else if (pos < w*(h-1)) {
		/* Horizontal edge. */
		y = pos / w;
		x = pos % w;
		dx = 0;
		dy = 1;
	    } else if (pos < 2*wh-w-h) {
		/* Vertical edge. */
		x = (pos - w*(h-1)) / h;
		y = (pos - w*(h-1)) % h;
		dx = 1;
		dy = 0;
	    } else
		return "Too much data in edge list";
	    if (!state)
		dsf_merge(map+wh, y*w+x, (y+dy)*w+(x+dx));

	    pos++;
	}
	if (*p != 'z')
	    state = !state;
	p++;
    }
    assert(pos <= 2*wh-w-h);
    if (pos < 2*wh-w-h)
	return "Too little data in edge list";

    /*
     * Now go through again and allocate region numbers.
     */
    pos = 0;
    for (i = 0; i < wh; i++)
	map[i] = -1;
    for (i = 0; i < wh; i++) {
	k = dsf_canonify(map+wh, i);
	if (map[k] < 0)
	    map[k] = pos++;
	map[i] = map[k];
    }
    if (pos != n)
	return "Edge list defines the wrong number of regions";

    *desc = p;

    return NULL;
}

static char *validate_desc(game_params *params, char *desc)
{
    int w = params->w, h = params->h, wh = w*h, n = params->n;
    int area;
    int *map;
    char *ret;

    map = snewn(2*wh, int);
    ret = parse_edge_list(params, &desc, map);
    if (ret)
	return ret;
    sfree(map);

    if (*desc != ',')
	return "Expected comma before clue list";
    desc++;			       /* eat comma */

    area = 0;
    while (*desc) {
	if (*desc >= '0' && *desc < '0'+FOUR)
	    area++;
	else if (*desc >= 'a' && *desc <= 'z')
	    area += *desc - 'a' + 1;
	else
	    return "Unexpected character in clue list";
	desc++;
    }
    if (area < n)
	return "Too little data in clue list";
    else if (area > n)
	return "Too much data in clue list";

    return NULL;
}

static game_state *new_game(midend *me, game_params *params, char *desc)
{
    int w = params->w, h = params->h, wh = w*h, n = params->n;
    int i, pos;
    char *p;
    game_state *state = snew(game_state);

    state->p = *params;
    state->colouring = snewn(n, int);
    for (i = 0; i < n; i++)
	state->colouring[i] = -1;

    state->completed = state->cheated = FALSE;

    state->map = snew(struct map);
    state->map->refcount = 1;
    state->map->map = snewn(wh*4, int);
    state->map->graph = snewn(n*n, int);
    state->map->n = n;
    state->map->immutable = snewn(n, int);
    for (i = 0; i < n; i++)
	state->map->immutable[i] = FALSE;

    p = desc;

    {
	char *ret;
	ret = parse_edge_list(params, &p, state->map->map);
	assert(!ret);
    }

    /*
     * Set up the other three quadrants in `map'.
     */
    for (i = wh; i < 4*wh; i++)
	state->map->map[i] = state->map->map[i % wh];

    assert(*p == ',');
    p++;

    /*
     * Now process the clue list.
     */
    pos = 0;
    while (*p) {
	if (*p >= '0' && *p < '0'+FOUR) {
	    state->colouring[pos] = *p - '0';
	    state->map->immutable[pos] = TRUE;
	    pos++;
	} else {
	    assert(*p >= 'a' && *p <= 'z');
	    pos += *p - 'a' + 1;
	}
	p++;
    }
    assert(pos == n);

    state->map->ngraph = gengraph(w, h, n, state->map->map, state->map->graph);

    /*
     * Attempt to smooth out some of the more jagged region
     * outlines by the judicious use of diagonally divided squares.
     */
    {
        random_state *rs = random_init(desc, strlen(desc));
        int *squares = snewn(wh, int);
        int done_something;

        for (i = 0; i < wh; i++)
            squares[i] = i;
        shuffle(squares, wh, sizeof(*squares), rs);

        do {
            done_something = FALSE;
            for (i = 0; i < wh; i++) {
                int y = squares[i] / w, x = squares[i] % w;
                int c = state->map->map[y*w+x];
                int tc, bc, lc, rc;

                if (x == 0 || x == w-1 || y == 0 || y == h-1)
                    continue;

                if (state->map->map[TE * wh + y*w+x] !=
                    state->map->map[BE * wh + y*w+x])
                    continue;

                tc = state->map->map[BE * wh + (y-1)*w+x];
                bc = state->map->map[TE * wh + (y+1)*w+x];
                lc = state->map->map[RE * wh + y*w+(x-1)];
                rc = state->map->map[LE * wh + y*w+(x+1)];

                /*
                 * If this square is adjacent on two sides to one
                 * region and on the other two sides to the other
                 * region, and is itself one of the two regions, we can
                 * adjust it so that it's a diagonal.
                 */
                if (tc != bc && (tc == c || bc == c)) {
                    if ((lc == tc && rc == bc) ||
                        (lc == bc && rc == tc)) {
                        state->map->map[TE * wh + y*w+x] = tc;
                        state->map->map[BE * wh + y*w+x] = bc;
                        state->map->map[LE * wh + y*w+x] = lc;
                        state->map->map[RE * wh + y*w+x] = rc;
                        done_something = TRUE;
                    }
                }
            }
        } while (done_something);
        sfree(squares);
        random_free(rs);
    }

    /*
     * Analyse the map to find a canonical line segment
     * corresponding to each edge. These are where we'll eventually
     * put error markers.
     */
    {
	int *bestx, *besty, *an, pass;
	float *ax, *ay, *best;

	ax = snewn(state->map->ngraph, float);
	ay = snewn(state->map->ngraph, float);
	an = snewn(state->map->ngraph, int);
	bestx = snewn(state->map->ngraph, int);
	besty = snewn(state->map->ngraph, int);
	best = snewn(state->map->ngraph, float);

	for (i = 0; i < state->map->ngraph; i++) {
	    bestx[i] = besty[i] = -1;
	    best[i] = 2*(w+h)+1;
	    ax[i] = ay[i] = 0.0F;
	    an[i] = 0;
	}

	/*
	 * We make two passes over the map, finding all the line
	 * segments separating regions. In the first pass, we
	 * compute the _average_ x and y coordinate of all the line
	 * segments separating each pair of regions; in the second
	 * pass, for each such average point, we find the line
	 * segment closest to it and call that canonical.
	 * 
	 * Line segments are considered to have coordinates in
	 * their centre. Thus, at least one coordinate for any line
	 * segment is always something-and-a-half; so we store our
	 * coordinates as twice their normal value.
	 */
	for (pass = 0; pass < 2; pass++) {
	    int x, y;

	    for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
		    int ex[3], ey[3], ea[3], eb[3], en = 0;

		    /*
		     * Look for an edge to the right of this
		     * square, an edge below it, and an edge in the
		     * middle of it.
		     */
		    if (x+1 < w) {
			/* right edge */
			ea[en] = state->map->map[RE * wh + y*w+x];
			eb[en] = state->map->map[LE * wh + y*w+(x+1)];
			if (ea[en] != eb[en]) {
			    ex[en] = (x+1)*2;
			    ey[en] = y*2+1;
			    en++;
			}
		    }
		    if (y+1 < h) {
			/* bottom edge */
			ea[en] = state->map->map[BE * wh + y*w+x];
			eb[en] = state->map->map[TE * wh + (y+1)*w+x];
			if (ea[en] != eb[en]) {
			    ex[en] = x*2+1;
			    ey[en] = (y+1)*2;
			    en++;
			}
		    }
		    /* diagonal edge */
		    ea[en] = state->map->map[TE * wh + y*w+x];
		    eb[en] = state->map->map[BE * wh + y*w+x];
		    if (ea[en] != eb[en]) {
			ex[en] = x*2+1;
			ey[en] = y*2+1;
			en++;
		    }

		    /*
		     * Now process the edges we've found, one by
		     * one.
		     */
		    for (i = 0; i < en; i++) {
			int emin = min(ea[i], eb[i]);
			int emax = max(ea[i], eb[i]);
			int gindex = 
			    graph_edge_index(state->map->graph, n,
					     state->map->ngraph, emin, emax);

			assert(gindex >= 0);

			if (pass == 0) {
			    /*
			     * In pass 0, accumulate the values
			     * we'll use to compute the average
			     * positions.
			     */
			    ax[gindex] += ex[i];
			    ay[gindex] += ey[i];
			    an[gindex] += 1.0F;
			} else {
			    /*
			     * In pass 1, work out whether this
			     * point is closer to the average than
			     * the last one we've seen.
			     */
			    float dx, dy, d;

			    assert(an[gindex] > 0);
			    dx = ex[i] - ax[gindex];
			    dy = ey[i] - ay[gindex];
			    d = sqrt(dx*dx + dy*dy);
			    if (d < best[gindex]) {
				best[gindex] = d;
				bestx[gindex] = ex[i];
				besty[gindex] = ey[i];
			    }
			}
		    }
		}

	    if (pass == 0) {
		for (i = 0; i < state->map->ngraph; i++)
		    if (an[i] > 0) {
			ax[i] /= an[i];
			ay[i] /= an[i];
		    }
	    }
	}

	state->map->edgex = bestx;
	state->map->edgey = besty;

	for (i = 0; i < state->map->ngraph; i++)
	    if (state->map->edgex[i] < 0) {
		/* Find the other representation of this edge. */
		int e = state->map->graph[i];
		int iprime = graph_edge_index(state->map->graph, n,
					      state->map->ngraph, e%n, e/n);
		assert(state->map->edgex[iprime] >= 0);
		state->map->edgex[i] = state->map->edgex[iprime];
		state->map->edgey[i] = state->map->edgey[iprime];
	    }

	sfree(ax);
	sfree(ay);
	sfree(an);
	sfree(best);
    }

    return state;
}

static game_state *dup_game(game_state *state)
{
    game_state *ret = snew(game_state);

    ret->p = state->p;
    ret->colouring = snewn(state->p.n, int);
    memcpy(ret->colouring, state->colouring, state->p.n * sizeof(int));
    ret->map = state->map;
    ret->map->refcount++;
    ret->completed = state->completed;
    ret->cheated = state->cheated;

    return ret;
}

static void free_game(game_state *state)
{
    if (--state->map->refcount <= 0) {
	sfree(state->map->map);
	sfree(state->map->graph);
	sfree(state->map->immutable);
	sfree(state->map->edgex);
	sfree(state->map->edgey);
	sfree(state->map);
    }
    sfree(state->colouring);
    sfree(state);
}

static char *solve_game(game_state *state, game_state *currstate,
			char *aux, char **error)
{
    if (!aux) {
	/*
	 * Use the solver.
	 */
	int *colouring;
	struct solver_scratch *sc;
	int sret;
	int i;
	char *ret, buf[80];
	int retlen, retsize;

	colouring = snewn(state->map->n, int);
	memcpy(colouring, state->colouring, state->map->n * sizeof(int));

	sc = new_scratch(state->map->graph, state->map->n, state->map->ngraph);
	sret = map_solver(sc, state->map->graph, state->map->n,
			 state->map->ngraph, colouring, DIFFCOUNT-1);
	free_scratch(sc);

	if (sret != 1) {
	    sfree(colouring);
	    if (sret == 0)
		*error = "Puzzle is inconsistent";
	    else
		*error = "Unable to find a unique solution for this puzzle";
	    return NULL;
	}

        retsize = 64;
        ret = snewn(retsize, char);
        strcpy(ret, "S");
        retlen = 1;

	for (i = 0; i < state->map->n; i++) {
            int len;

	    assert(colouring[i] >= 0);
            if (colouring[i] == currstate->colouring[i])
                continue;
	    assert(!state->map->immutable[i]);

            len = sprintf(buf, ";%d:%d", colouring[i], i);
            if (retlen + len >= retsize) {
                retsize = retlen + len + 256;
                ret = sresize(ret, retsize, char);
            }
            strcpy(ret + retlen, buf);
            retlen += len;
        }

	sfree(colouring);

	return ret;
    }
    return dupstr(aux);
}

static char *game_text_format(game_state *state)
{
    return NULL;
}

struct game_ui {
    int drag_colour;                   /* -1 means no drag active */
    int dragx, dragy;
};

static game_ui *new_ui(game_state *state)
{
    game_ui *ui = snew(game_ui);
    ui->dragx = ui->dragy = -1;
    ui->drag_colour = -2;
    return ui;
}

static void free_ui(game_ui *ui)
{
    sfree(ui);
}

static char *encode_ui(game_ui *ui)
{
    return NULL;
}

static void decode_ui(game_ui *ui, char *encoding)
{
}

static void game_changed_state(game_ui *ui, game_state *oldstate,
                               game_state *newstate)
{
}

struct game_drawstate {
    int tilesize;
    unsigned short *drawn, *todraw;
    int started;
    int dragx, dragy, drag_visible;
    blitter *bl;
};

/* Flags in `drawn'. */
#define ERR_T    0x0100
#define ERR_B    0x0200
#define ERR_L    0x0400
#define ERR_R    0x0800
#define ERR_C    0x1000
#define ERR_MASK 0x1F00

#define TILESIZE (ds->tilesize)
#define BORDER (TILESIZE)
#define COORD(x)  ( (x) * TILESIZE + BORDER )
#define FROMCOORD(x)  ( ((x) - BORDER + TILESIZE) / TILESIZE - 1 )

static int region_from_coords(game_state *state, game_drawstate *ds,
                              int x, int y)
{
    int w = state->p.w, h = state->p.h, wh = w*h /*, n = state->p.n */;
    int tx = FROMCOORD(x), ty = FROMCOORD(y);
    int dx = x - COORD(tx), dy = y - COORD(ty);
    int quadrant;

    if (tx < 0 || tx >= w || ty < 0 || ty >= h)
        return -1;                     /* border */

    quadrant = 2 * (dx > dy) + (TILESIZE - dx > dy);
    quadrant = (quadrant == 0 ? BE :
                quadrant == 1 ? LE :
                quadrant == 2 ? RE : TE);

    return state->map->map[quadrant * wh + ty*w+tx];
}

static char *interpret_move(game_state *state, game_ui *ui, game_drawstate *ds,
			    int x, int y, int button)
{
    char buf[80];

    if (button == LEFT_BUTTON || button == RIGHT_BUTTON) {
	int r = region_from_coords(state, ds, x, y);

        if (r >= 0)
            ui->drag_colour = state->colouring[r];
        else
            ui->drag_colour = -1;
        ui->dragx = x;
        ui->dragy = y;
        return "";
    }

    if ((button == LEFT_DRAG || button == RIGHT_DRAG) &&
        ui->drag_colour > -2) {
        ui->dragx = x;
        ui->dragy = y;
        return "";
    }

    if ((button == LEFT_RELEASE || button == RIGHT_RELEASE) &&
        ui->drag_colour > -2) {
	int r = region_from_coords(state, ds, x, y);
        int c = ui->drag_colour;

        /*
         * Cancel the drag, whatever happens.
         */
        ui->drag_colour = -2;
        ui->dragx = ui->dragy = -1;

	if (r < 0)
            return "";                 /* drag into border; do nothing else */

	if (state->map->immutable[r])
	    return "";                 /* can't change this region */

        if (state->colouring[r] == c)
            return "";                 /* don't _need_ to change this region */

	sprintf(buf, "%c:%d", (int)(c < 0 ? 'C' : '0' + c), r);
	return dupstr(buf);
    }

    return NULL;
}

static game_state *execute_move(game_state *state, char *move)
{
    int n = state->p.n;
    game_state *ret = dup_game(state);
    int c, k, adv, i;

    while (*move) {
	c = *move;
	if ((c == 'C' || (c >= '0' && c < '0'+FOUR)) &&
	    sscanf(move+1, ":%d%n", &k, &adv) == 1 &&
	    k >= 0 && k < state->p.n) {
	    move += 1 + adv;
	    ret->colouring[k] = (c == 'C' ? -1 : c - '0');
	} else if (*move == 'S') {
	    move++;
	    ret->cheated = TRUE;
	} else {
	    free_game(ret);
	    return NULL;
	}

	if (*move && *move != ';') {
	    free_game(ret);
	    return NULL;
	}
	if (*move)
	    move++;
    }

    /*
     * Check for completion.
     */
    if (!ret->completed) {
	int ok = TRUE;

	for (i = 0; i < n; i++)
	    if (ret->colouring[i] < 0) {
		ok = FALSE;
		break;
	    }

	if (ok) {
	    for (i = 0; i < ret->map->ngraph; i++) {
		int j = ret->map->graph[i] / n;
		int k = ret->map->graph[i] % n;
		if (ret->colouring[j] == ret->colouring[k]) {
		    ok = FALSE;
		    break;
		}
	    }
	}

	if (ok)
	    ret->completed = TRUE;
    }

    return ret;
}

/* ----------------------------------------------------------------------
 * Drawing routines.
 */

static void game_compute_size(game_params *params, int tilesize,
			      int *x, int *y)
{
    /* Ick: fake up `ds->tilesize' for macro expansion purposes */
    struct { int tilesize; } ads, *ds = &ads;
    ads.tilesize = tilesize;

    *x = params->w * TILESIZE + 2 * BORDER + 1;
    *y = params->h * TILESIZE + 2 * BORDER + 1;
}

static void game_set_size(drawing *dr, game_drawstate *ds,
			  game_params *params, int tilesize)
{
    ds->tilesize = tilesize;

    if (ds->bl)
        blitter_free(dr, ds->bl);
    ds->bl = blitter_new(dr, TILESIZE+3, TILESIZE+3);
}

const float map_colours[FOUR][3] = {
    {0.7F, 0.5F, 0.4F},
    {0.8F, 0.7F, 0.4F},
    {0.5F, 0.6F, 0.4F},
    {0.55F, 0.45F, 0.35F},
};
const int map_hatching[FOUR] = {
    HATCH_VERT, HATCH_SLASH, HATCH_HORIZ, HATCH_BACKSLASH
};

static float *game_colours(frontend *fe, game_state *state, int *ncolours)
{
    float *ret = snewn(3 * NCOLOURS, float);

    frontend_default_colour(fe, &ret[COL_BACKGROUND * 3]);

    ret[COL_GRID * 3 + 0] = 0.0F;
    ret[COL_GRID * 3 + 1] = 0.0F;
    ret[COL_GRID * 3 + 2] = 0.0F;

    memcpy(ret + COL_0 * 3, map_colours[0], 3 * sizeof(float));
    memcpy(ret + COL_1 * 3, map_colours[1], 3 * sizeof(float));
    memcpy(ret + COL_2 * 3, map_colours[2], 3 * sizeof(float));
    memcpy(ret + COL_3 * 3, map_colours[3], 3 * sizeof(float));

    ret[COL_ERROR * 3 + 0] = 1.0F;
    ret[COL_ERROR * 3 + 1] = 0.0F;
    ret[COL_ERROR * 3 + 2] = 0.0F;

    ret[COL_ERRTEXT * 3 + 0] = 1.0F;
    ret[COL_ERRTEXT * 3 + 1] = 1.0F;
    ret[COL_ERRTEXT * 3 + 2] = 1.0F;

    *ncolours = NCOLOURS;
    return ret;
}

static game_drawstate *game_new_drawstate(drawing *dr, game_state *state)
{
    struct game_drawstate *ds = snew(struct game_drawstate);
    int i;

    ds->tilesize = 0;
    ds->drawn = snewn(state->p.w * state->p.h, unsigned short);
    for (i = 0; i < state->p.w * state->p.h; i++)
	ds->drawn[i] = 0xFFFF;
    ds->todraw = snewn(state->p.w * state->p.h, unsigned short);
    ds->started = FALSE;
    ds->bl = NULL;
    ds->drag_visible = FALSE;
    ds->dragx = ds->dragy = -1;

    return ds;
}

static void game_free_drawstate(drawing *dr, game_drawstate *ds)
{
    sfree(ds->drawn);
    sfree(ds->todraw);
    if (ds->bl)
        blitter_free(dr, ds->bl);
    sfree(ds);
}

static void draw_error(drawing *dr, game_drawstate *ds, int x, int y)
{
    int coords[8];
    int yext, xext;

    /*
     * Draw a diamond.
     */
    coords[0] = x - TILESIZE*2/5;
    coords[1] = y;
    coords[2] = x;
    coords[3] = y - TILESIZE*2/5;
    coords[4] = x + TILESIZE*2/5;
    coords[5] = y;
    coords[6] = x;
    coords[7] = y + TILESIZE*2/5;
    draw_polygon(dr, coords, 4, COL_ERROR, COL_GRID);

    /*
     * Draw an exclamation mark in the diamond. This turns out to
     * look unpleasantly off-centre if done via draw_text, so I do
     * it by hand on the basis that exclamation marks aren't that
     * difficult to draw...
     */
    xext = TILESIZE/16;
    yext = TILESIZE*2/5 - (xext*2+2);
    draw_rect(dr, x-xext, y-yext, xext*2+1, yext*2+1 - (xext*3+1),
	      COL_ERRTEXT);
    draw_rect(dr, x-xext, y+yext-xext*2, xext*2+1, xext*2+1, COL_ERRTEXT);
}

static void draw_square(drawing *dr, game_drawstate *ds,
			game_params *params, struct map *map,
			int x, int y, int v)
{
    int w = params->w, h = params->h, wh = w*h;
    int tv, bv, errs;

    errs = v & ERR_MASK;
    v &= ~ERR_MASK;
    tv = v / FIVE;
    bv = v % FIVE;

    clip(dr, COORD(x), COORD(y), TILESIZE, TILESIZE);

    /*
     * Draw the region colour.
     */
    draw_rect(dr, COORD(x), COORD(y), TILESIZE, TILESIZE,
	      (tv == FOUR ? COL_BACKGROUND : COL_0 + tv));
    /*
     * Draw the second region colour, if this is a diagonally
     * divided square.
     */
    if (map->map[TE * wh + y*w+x] != map->map[BE * wh + y*w+x]) {
        int coords[6];
        coords[0] = COORD(x)-1;
        coords[1] = COORD(y+1)+1;
        if (map->map[LE * wh + y*w+x] == map->map[TE * wh + y*w+x])
            coords[2] = COORD(x+1)+1;
        else
            coords[2] = COORD(x)-1;
        coords[3] = COORD(y)-1;
        coords[4] = COORD(x+1)+1;
        coords[5] = COORD(y+1)+1;
        draw_polygon(dr, coords, 3,
                     (bv == FOUR ? COL_BACKGROUND : COL_0 + bv), COL_GRID);
    }

    /*
     * Draw the grid lines, if required.
     */
    if (x <= 0 || map->map[RE*wh+y*w+(x-1)] != map->map[LE*wh+y*w+x])
	draw_rect(dr, COORD(x), COORD(y), 1, TILESIZE, COL_GRID);
    if (y <= 0 || map->map[BE*wh+(y-1)*w+x] != map->map[TE*wh+y*w+x])
	draw_rect(dr, COORD(x), COORD(y), TILESIZE, 1, COL_GRID);
    if (x <= 0 || y <= 0 ||
        map->map[RE*wh+(y-1)*w+(x-1)] != map->map[TE*wh+y*w+x] ||
        map->map[BE*wh+(y-1)*w+(x-1)] != map->map[LE*wh+y*w+x])
	draw_rect(dr, COORD(x), COORD(y), 1, 1, COL_GRID);

    /*
     * Draw error markers.
     */
    if (errs & ERR_T)
	draw_error(dr, ds, COORD(x)+TILESIZE/2, COORD(y));
    if (errs & ERR_L)
	draw_error(dr, ds, COORD(x), COORD(y)+TILESIZE/2);
    if (errs & ERR_B)
	draw_error(dr, ds, COORD(x)+TILESIZE/2, COORD(y+1));
    if (errs & ERR_R)
	draw_error(dr, ds, COORD(x+1), COORD(y)+TILESIZE/2);
    if (errs & ERR_C)
	draw_error(dr, ds, COORD(x)+TILESIZE/2, COORD(y)+TILESIZE/2);

    unclip(dr);

    draw_update(dr, COORD(x), COORD(y), TILESIZE, TILESIZE);
}

static void game_redraw(drawing *dr, game_drawstate *ds, game_state *oldstate,
			game_state *state, int dir, game_ui *ui,
			float animtime, float flashtime)
{
    int w = state->p.w, h = state->p.h, wh = w*h, n = state->p.n;
    int x, y, i;
    int flash;

    if (ds->drag_visible) {
        blitter_load(dr, ds->bl, ds->dragx, ds->dragy);
        draw_update(dr, ds->dragx, ds->dragy, TILESIZE + 3, TILESIZE + 3);
        ds->drag_visible = FALSE;
    }

    /*
     * The initial contents of the window are not guaranteed and
     * can vary with front ends. To be on the safe side, all games
     * should start by drawing a big background-colour rectangle
     * covering the whole window.
     */
    if (!ds->started) {
	int ww, wh;

	game_compute_size(&state->p, TILESIZE, &ww, &wh);
	draw_rect(dr, 0, 0, ww, wh, COL_BACKGROUND);
	draw_rect(dr, COORD(0), COORD(0), w*TILESIZE+1, h*TILESIZE+1,
		  COL_GRID);

	draw_update(dr, 0, 0, ww, wh);
	ds->started = TRUE;
    }

    if (flashtime) {
	if (flash_type == 1)
	    flash = (int)(flashtime * FOUR / flash_length);
	else
	    flash = 1 + (int)(flashtime * THREE / flash_length);
    } else
	flash = -1;

    /*
     * Set up the `todraw' array.
     */
    for (y = 0; y < h; y++)
	for (x = 0; x < w; x++) {
	    int tv = state->colouring[state->map->map[TE * wh + y*w+x]];
	    int bv = state->colouring[state->map->map[BE * wh + y*w+x]];
            int v;

	    if (tv < 0)
		tv = FOUR;
	    if (bv < 0)
		bv = FOUR;

	    if (flash >= 0) {
		if (flash_type == 1) {
		    if (tv == flash)
			tv = FOUR;
		    if (bv == flash)
			bv = FOUR;
		} else if (flash_type == 2) {
		    if (flash % 2)
			tv = bv = FOUR;
		} else {
		    if (tv != FOUR)
			tv = (tv + flash) % FOUR;
		    if (bv != FOUR)
			bv = (bv + flash) % FOUR;
		}
	    }

            v = tv * FIVE + bv;

	    ds->todraw[y*w+x] = v;
	}

    /*
     * Add error markers to the `todraw' array.
     */
    for (i = 0; i < state->map->ngraph; i++) {
	int v1 = state->map->graph[i] / n;
	int v2 = state->map->graph[i] % n;

	if (state->colouring[v1] < 0 || state->colouring[v2] < 0)
	    continue;
	if (state->colouring[v1] != state->colouring[v2])
	    continue;

	x = state->map->edgex[i];
	y = state->map->edgey[i];

	if (x % 2 && y % 2) {
	    ds->todraw[(y/2)*w+(x/2)] |= ERR_C;
	} else if (x % 2) {
	    ds->todraw[(y/2-1)*w+(x/2)] |= ERR_B;
	    ds->todraw[(y/2)*w+(x/2)] |= ERR_T;
	} else {
	    assert(y % 2);
	    ds->todraw[(y/2)*w+(x/2-1)] |= ERR_R;
	    ds->todraw[(y/2)*w+(x/2)] |= ERR_L;
	}
    }

    /*
     * Now actually draw everything.
     */
    for (y = 0; y < h; y++)
	for (x = 0; x < w; x++) {
	    int v = ds->todraw[y*w+x];
	    if (ds->drawn[y*w+x] != v) {
		draw_square(dr, ds, &state->p, state->map, x, y, v);
		ds->drawn[y*w+x] = v;
	    }
	}

    /*
     * Draw the dragged colour blob if any.
     */
    if (ui->drag_colour > -2) {
        ds->dragx = ui->dragx - TILESIZE/2 - 2;
        ds->dragy = ui->dragy - TILESIZE/2 - 2;
        blitter_save(dr, ds->bl, ds->dragx, ds->dragy);
        draw_circle(dr, ui->dragx, ui->dragy, TILESIZE/2,
                    (ui->drag_colour < 0 ? COL_BACKGROUND :
                     COL_0 + ui->drag_colour), COL_GRID);
        draw_update(dr, ds->dragx, ds->dragy, TILESIZE + 3, TILESIZE + 3);
        ds->drag_visible = TRUE;
    }
}

static float game_anim_length(game_state *oldstate, game_state *newstate,
			      int dir, game_ui *ui)
{
    return 0.0F;
}

static float game_flash_length(game_state *oldstate, game_state *newstate,
			       int dir, game_ui *ui)
{
    if (!oldstate->completed && newstate->completed &&
	!oldstate->cheated && !newstate->cheated) {
	if (flash_type < 0) {
	    char *env = getenv("MAP_ALTERNATIVE_FLASH");
	    if (env)
		flash_type = atoi(env);
	    else
		flash_type = 0;
	    flash_length = (flash_type == 1 ? 0.50 : 0.30);
	}
	return flash_length;
    } else
	return 0.0F;
}

static int game_wants_statusbar(void)
{
    return FALSE;
}

static int game_timing_state(game_state *state, game_ui *ui)
{
    return TRUE;
}

static void game_print_size(game_params *params, float *x, float *y)
{
    int pw, ph;

    /*
     * I'll use 4mm squares by default, I think. Simplest way to
     * compute this size is to compute the pixel puzzle size at a
     * given tile size and then scale.
     */
    game_compute_size(params, 400, &pw, &ph);
    *x = pw / 100.0;
    *y = ph / 100.0;
}

static void game_print(drawing *dr, game_state *state, int tilesize)
{
    int w = state->p.w, h = state->p.h, wh = w*h, n = state->p.n;
    int ink, c[FOUR], i;
    int x, y, r;
    int *coords, ncoords, coordsize;

    /* Ick: fake up `ds->tilesize' for macro expansion purposes */
    struct { int tilesize; } ads, *ds = &ads;
    ads.tilesize = tilesize;

    ink = print_mono_colour(dr, 0);
    for (i = 0; i < FOUR; i++)
	c[i] = print_rgb_colour(dr, map_hatching[i], map_colours[i][0],
				map_colours[i][1], map_colours[i][2]);

    coordsize = 0;
    coords = NULL;

    print_line_width(dr, TILESIZE / 16);

    /*
     * Draw a single filled polygon around each region.
     */
    for (r = 0; r < n; r++) {
	int octants[8], lastdir, d1, d2, ox, oy;

	/*
	 * Start by finding a point on the region boundary. Any
	 * point will do. To do this, we'll search for a square
	 * containing the region and then decide which corner of it
	 * to use.
	 */
	x = w;
	for (y = 0; y < h; y++) {
	    for (x = 0; x < w; x++) {
		if (state->map->map[wh*0+y*w+x] == r ||
		    state->map->map[wh*1+y*w+x] == r ||
		    state->map->map[wh*2+y*w+x] == r ||
		    state->map->map[wh*3+y*w+x] == r)
		    break;
	    }
	    if (x < w)
		break;
	}
	assert(y < h && x < w);	       /* we must have found one somewhere */
	/*
	 * This is the first square in lexicographic order which
	 * contains part of this region. Therefore, one of the top
	 * two corners of the square must be what we're after. The
	 * only case in which it isn't the top left one is if the
	 * square is diagonally divided and the region is in the
	 * bottom right half.
	 */
	if (state->map->map[wh*TE+y*w+x] != r &&
	    state->map->map[wh*LE+y*w+x] != r)
	    x++;		       /* could just as well have done y++ */

	/*
	 * Now we have a point on the region boundary. Trace around
	 * the region until we come back to this point,
	 * accumulating coordinates for a polygon draw operation as
	 * we go.
	 */
	lastdir = -1;
	ox = x;
	oy = y;
	ncoords = 0;

	do {
	    /*
	     * There are eight possible directions we could head in
	     * from here. We identify them by octant numbers, and
	     * we also use octant numbers to identify the spaces
	     * between them:
	     * 
	     *   6   7   0
	     *    \ 7|0 /
	     *     \ | /
	     *    6 \|/ 1
	     * 5-----+-----1
	     *    5 /|\ 2
	     *     / | \
	     *    / 4|3 \
	     *   4   3   2
	     */
	    octants[0] = x<w && y>0 ? state->map->map[wh*LE+(y-1)*w+x] : -1;
	    octants[1] = x<w && y>0 ? state->map->map[wh*BE+(y-1)*w+x] : -1;
	    octants[2] = x<w && y<h ? state->map->map[wh*TE+y*w+x] : -1;
	    octants[3] = x<w && y<h ? state->map->map[wh*LE+y*w+x] : -1;
	    octants[4] = x>0 && y<h ? state->map->map[wh*RE+y*w+(x-1)] : -1;
	    octants[5] = x>0 && y<h ? state->map->map[wh*TE+y*w+(x-1)] : -1;
	    octants[6] = x>0 && y>0 ? state->map->map[wh*BE+(y-1)*w+(x-1)] :-1;
	    octants[7] = x>0 && y>0 ? state->map->map[wh*RE+(y-1)*w+(x-1)] :-1;

	    d1 = d2 = -1;
	    for (i = 0; i < 8; i++)
		if ((octants[i] == r) ^ (octants[(i+1)%8] == r)) {
		    assert(d2 == -1);
		    if (d1 == -1)
			d1 = i;
		    else
			d2 = i;
		}
/* printf("%% %d,%d r=%d: d1=%d d2=%d lastdir=%d\n", x, y, r, d1, d2, lastdir); */
	    assert(d1 != -1 && d2 != -1);
	    if (d1 == lastdir)
		d1 = d2;

	    /*
	     * Now we're heading in direction d1. Save the current
	     * coordinates.
	     */
	    if (ncoords + 2 > coordsize) {
		coordsize += 128;
		coords = sresize(coords, coordsize, int);
	    }
	    coords[ncoords++] = COORD(x);
	    coords[ncoords++] = COORD(y);

	    /*
	     * Compute the new coordinates.
	     */
	    x += (d1 % 4 == 3 ? 0 : d1 < 4 ? +1 : -1);
	    y += (d1 % 4 == 1 ? 0 : d1 > 1 && d1 < 5 ? +1 : -1);
	    assert(x >= 0 && x <= w && y >= 0 && y <= h);

	    lastdir = d1 ^ 4;
	} while (x != ox || y != oy);

	draw_polygon(dr, coords, ncoords/2,
		     state->colouring[r] >= 0 ?
		     c[state->colouring[r]] : -1, ink);
    }
    sfree(coords);
}

#ifdef COMBINED
#define thegame map
#endif

const struct game thegame = {
    "Map", "games.map",
    default_params,
    game_fetch_preset,
    decode_params,
    encode_params,
    free_params,
    dup_params,
    TRUE, game_configure, custom_params,
    validate_params,
    new_game_desc,
    validate_desc,
    new_game,
    dup_game,
    free_game,
    TRUE, solve_game,
    FALSE, game_text_format,
    new_ui,
    free_ui,
    encode_ui,
    decode_ui,
    game_changed_state,
    interpret_move,
    execute_move,
    20, game_compute_size, game_set_size,
    game_colours,
    game_new_drawstate,
    game_free_drawstate,
    game_redraw,
    game_anim_length,
    game_flash_length,
    TRUE, TRUE, game_print_size, game_print,
    game_wants_statusbar,
    FALSE, game_timing_state,
    0,				       /* mouse_priorities */
};
