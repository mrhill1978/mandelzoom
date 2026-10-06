// mandelzoom: endless Mandelbrot zoom for the terminal.
//
// In terminals that speak the kitty graphics protocol (kitty, Ghostty) it
// draws real pixels, handing each frame over through shared memory or a
// temp file and adjusting the render resolution to hold the frame rate.
// Elsewhere it falls back to anti-aliased upper-half-block characters.
//
// It zooms into a list of known-interesting points and moves to the next
// one when the view hits double-precision limits or turns into a flat color.
// Any key quits.
//
// Build: cc -O3 -march=native -fopenmp -o mandelzoom mandelzoom.c -lm
// Usage: mandelzoom [-s zoom_per_frame] [-f fps] [-r downscale] [-b] [-v]

#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const struct { double x, y; } targets[] = {
	{ -0.743643887037151,  0.131825904205330 }, // seahorse valley
	{  0.001643721971153, -0.822467633298876 },
	{ -0.7746806106269039, -0.1374168856037867 },
	{ -0.10109636384562,   0.95628651080914 },
	{ -1.768778833,       -0.001738996 },
};
#define NTARGETS (sizeof targets / sizeof targets[0])

#define START_SCALE 3.0   // width of the view in the complex plane
#define MIN_SCALE   1e-12 // past this, doubles start to show blocks

// Render-resolution divisors for graphics mode, picked adaptively.
static const double levels[] = { 1, 1.5, 2, 3, 4, 6, 8 };
#define NLEVELS (sizeof levels / sizeof levels[0])

enum mode { BLOCKS, GFX_SHM, GFX_FILE };
static enum mode mode = BLOCKS;

static struct termios orig_tio;
static volatile sig_atomic_t stop;

// Frames handed to the terminal, unlinked once they are RING frames old in
// case the terminal didn't remove them itself.
#define RING 64
static struct { char kind; char name[160]; } ring[RING];

static int pending, oks, errs; // graphics replies outstanding / received
static char quit_bytes[64];    // input that ended the run, for -v
static int quit_len;

static void on_signal(int sig) { (void)sig; stop = 1; }

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void unlink_slot(int i)
{
	if (ring[i].kind == 's')
		shm_unlink(ring[i].name);
	else if (ring[i].kind == 't')
		unlink(ring[i].name);
	ring[i].kind = 0;
}

static void restore_term(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	if (mode != BLOCKS)
		fputs("\x1b_Ga=d,d=A,q=2\x1b\\", stdout);
	fputs("\x1b[0m\x1b[2J\x1b[?25h\x1b[?1049l", stdout);
	fflush(stdout);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
	for (int i = 0; i < RING; i++)
		unlink_slot(i);
}

static void base64(const char *in, char *out)
{
	static const char tab[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t n = strlen(in);
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned char)in[i] << 16;
		if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
		if (i + 2 < n) v |= (unsigned char)in[i + 2];
		*out++ = tab[v >> 18 & 63];
		*out++ = tab[v >> 12 & 63];
		*out++ = i + 1 < n ? tab[v >> 6 & 63] : '=';
		*out++ = i + 2 < n ? tab[v & 63] : '=';
	}
	*out = 0;
}

// Writes raw pixel data where the terminal can pick it up: a POSIX shared
// memory object ('s') or a temp file ('t'). kitty only accepts temp files
// whose name contains "tty-graphics-protocol".
static int store_pixels(char kind, unsigned seq, const unsigned char *data,
                        size_t size, char *name, size_t namesz)
{
	int fd;
	if (kind == 's') {
		snprintf(name, namesz, "/mandelzoom-%d-%u", (int)getpid(), seq);
		fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	} else {
		const char *tmp = getenv("TMPDIR");
		if (!tmp || !*tmp)
			tmp = "/tmp";
		snprintf(name, namesz, "%s/tty-graphics-protocol-mandelzoom-%d-%u",
		         tmp, (int)getpid(), seq);
		fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
	}
	if (fd < 0)
		return -1;
	while (size) {
		ssize_t n = write(fd, data, size);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		data += n;
		size -= n;
	}
	close(fd);
	return 0;
}

// Asks the terminal whether it can load images from shared memory or a temp
// file. Terminals without the kitty graphics protocol ignore the queries and
// only answer the device-attributes request sent after them.
static enum mode probe(void)
{
	static const unsigned char px[3];
	char shm[160], file[160], b64[256], out[1024], resp[4096];
	int have_shm = store_pixels('s', 0, px, 3, shm, sizeof shm) == 0;
	int have_file = store_pixels('t', 0, px, 3, file, sizeof file) == 0;
	int n = 0;
	if (have_shm) {
		base64(shm, b64);
		n += sprintf(out + n, "\x1b_Gi=31,a=q,t=s,f=24,s=1,v=1;%s\x1b\\", b64);
	}
	if (have_file) {
		base64(file, b64);
		n += sprintf(out + n, "\x1b_Gi=32,a=q,t=t,f=24,s=1,v=1;%s\x1b\\", b64);
	}
	n += sprintf(out + n, "\x1b[c");
	if (write(STDOUT_FILENO, out, n) != n)
		n = 0;

	int len = 0;
	double deadline = now() + 1.0;
	while (n && len < (int)sizeof resp - 1) {
		int ms = (int)((deadline - now()) * 1000);
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		if (ms <= 0 || poll(&pfd, 1, ms) <= 0)
			break;
		ssize_t r = read(STDIN_FILENO, resp + len, sizeof resp - 1 - len);
		if (r <= 0)
			break;
		len += r;
		resp[len] = 0;
		char *da = strstr(resp, "\x1b[?");
		if (da && strchr(da, 'c'))
			break;
	}
	resp[len] = 0;
	if (have_shm)
		shm_unlink(shm);
	if (have_file)
		unlink(file);
	if (strstr(resp, "i=31;OK"))
		return GFX_SHM;
	if (strstr(resp, "i=32;OK"))
		return GFX_FILE;
	return BLOCKS;
}

// Waits up to timeout_ms for input and consumes it. Graphics-protocol
// replies are counted; anything else is a keypress. Returns 1 on a keypress.
static int read_input(int timeout_ms)
{
	static char buf[1024];
	static int len;
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	if (poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0) <= 0)
		return 0;
	ssize_t n = read(STDIN_FILENO, buf + len, sizeof buf - len);
	if (n <= 0)
		return 1;
	len += n;
	int i = 0;
	while (i < len) {
		if (buf[i] != '\x1b' || i + 1 == len || buf[i + 1] != '_') {
			quit_len = len - i < (int)sizeof quit_bytes ? len - i : (int)sizeof quit_bytes;
			memcpy(quit_bytes, buf + i, quit_len);
			return 1;
		}
		char *end = memmem(buf + i + 2, len - i - 2, "\x1b\\", 2);
		if (!end)
			break;
		if (memmem(buf + i, end - (buf + i), ";OK", 3))
			oks++;
		else
			errs++;
		if (pending > 0)
			pending--;
		i = end - buf + 2;
	}
	memmove(buf, buf + i, len - i);
	len -= i;
	if (len == sizeof buf)
		len = 0;
	return 0;
}

// Smooth (fractional) escape count, or -1 for points inside the set.
static double escape(double cr, double ci, int maxit)
{
	// Skip the main cardioid and period-2 bulb; they never escape.
	double q = (cr - 0.25) * (cr - 0.25) + ci * ci;
	if (q * (q + (cr - 0.25)) <= 0.25 * ci * ci ||
	    (cr + 1) * (cr + 1) + ci * ci <= 0.0625)
		return -1;

	double zr = 0, zi = 0, zr2 = 0, zi2 = 0;
	for (int n = 0; n < maxit; n++) {
		zi = 2 * zr * zi + ci;
		zr = zr2 - zi2 + cr;
		zr2 = zr * zr;
		zi2 = zi * zi;
		if (zr2 + zi2 > 256.0)
			return n + 1 - log2(0.5 * log(zr2 + zi2));
	}
	return -1;
}

// Cosine palette; phase drifts over time so colors keep flowing.
static void shade(double mu, double phase, int rgb[3])
{
	if (mu < 0) {
		rgb[0] = rgb[1] = rgb[2] = 0;
		return;
	}
	static const double off[3] = { 0.00, 0.10, 0.20 };
	double t = 0.04 * mu + phase;
	for (int c = 0; c < 3; c++)
		rgb[c] = (int)(255 * (0.5 + 0.5 * cos(6.283185307179586 * (t + off[c]))));
}

// A frame is "boring" once nearly every pixel lands in the same band.
static int boring(const double *mu, long n, int maxit)
{
	long *hist = calloc(maxit + 2, sizeof *hist);
	long top = 0;
	for (long i = 0; i < n; i++) {
		int b = mu[i] < 0 ? maxit + 1 : (int)mu[i];
		if (b < 0) b = 0;
		if (b > maxit + 1) b = maxit + 1;
		if (++hist[b] > top) top = hist[b];
	}
	free(hist);
	return top > 0.92 * n;
}

static void *grow(void *p, size_t *cap, size_t need)
{
	if (need <= *cap)
		return p;
	free(p);
	*cap = need;
	p = malloc(need);
	if (!p) {
		perror("malloc");
		exit(1);
	}
	return p;
}

// Half-block fallback. mu holds 2x2 samples per pixel, two pixels per cell.
static size_t draw_blocks(char *out, const double *mu, int cols, int rows, double phase)
{
	int gw = cols * 2;
	char *p = out;
	for (int r = 0; r < rows; r++) {
		p += sprintf(p, "\x1b[%d;1H", r + 1);
		int fg[3] = { -1, -1, -1 }, bg[3] = { -1, -1, -1 };
		for (int x = 0; x < cols; x++) {
			int px[2][3];
			for (int half = 0; half < 2; half++) {
				int sum[3] = { 0, 0, 0 }, c[3];
				int sy = (2 * r + half) * 2;
				for (int dy = 0; dy < 2; dy++)
					for (int dx = 0; dx < 2; dx++) {
						shade(mu[(sy + dy) * gw + 2 * x + dx], phase, c);
						for (int k = 0; k < 3; k++)
							sum[k] += c[k];
					}
				for (int k = 0; k < 3; k++)
					px[half][k] = sum[k] / 4;
			}
			if (memcmp(px[0], fg, sizeof fg)) {
				p += sprintf(p, "\x1b[38;2;%d;%d;%dm", px[0][0], px[0][1], px[0][2]);
				memcpy(fg, px[0], sizeof fg);
			}
			if (memcmp(px[1], bg, sizeof bg)) {
				p += sprintf(p, "\x1b[48;2;%d;%d;%dm", px[1][0], px[1][1], px[1][2]);
				memcpy(bg, px[1], sizeof bg);
			}
			memcpy(p, "\xe2\x96\x80", 3); // U+2580 upper half block
			p += 3;
		}
	}
	return p - out;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -s ZOOM   zoom factor per frame, 0.9-0.999 (default 0.985)\n"
		"  -f FPS    target frame rate (default 30)\n"
		"  -r N      fixed render downscale in graphics mode, 1 = full res\n"
		"            (default: adapt to hold the frame rate)\n"
		"  -b        force text blocks even if the terminal can show images\n"
		"  -v        print mode and stats on exit\n", argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	double zoom = 0.985, fps = 30, fixed_div = 0;
	int force_blocks = 0, verbose = 0, opt;
	while ((opt = getopt(argc, argv, "s:f:r:bvh")) != -1) {
		switch (opt) {
		case 's': zoom = atof(optarg); break;
		case 'f': fps = atof(optarg); break;
		case 'r': fixed_div = atof(optarg); if (fixed_div < 1) usage(argv[0]); break;
		case 'b': force_blocks = 1; break;
		case 'v': verbose = 1; break;
		default: usage(argv[0]);
		}
	}
	if (zoom <= 0.5 || zoom >= 1 || fps <= 0)
		usage(argv[0]);
	if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) {
		fprintf(stderr, "%s: needs a terminal\n", argv[0]);
		return 1;
	}

	tcgetattr(STDIN_FILENO, &orig_tio);
	struct termios raw = orig_tio;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	atexit(restore_term);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);

	if (!force_blocks)
		mode = probe();
	enum mode first_mode = mode;
	fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);

	srand(time(NULL));
	size_t ti = rand() % NTARGETS;
	double scale = START_SCALE;
	int frames_on_target = 0;

	double *mu = NULL;
	unsigned char *rgb = NULL;
	char *out = NULL;
	size_t mu_cap = 0, rgb_cap = 0, out_cap = 0;
	int cols = 0, rows = 0, gw = 0, gh = 0;
	int li = 2, over = 0, under = 0; // adaptive resolution state
	int img_id = 0;
	unsigned seq = 0;
	long frames = 0;
	double t0 = now();

	while (!stop) {
		double frame_start = now();

		struct winsize ws = { 0 };
		ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
		int ncols = ws.ws_col ? ws.ws_col : 80;
		int nrows = ws.ws_row ? ws.ws_row : 24;
		if ((ncols != cols || nrows != rows) && mode == BLOCKS)
			fputs("\x1b[2J", stdout);
		cols = ncols;
		rows = nrows;

		double div = fixed_div ? fixed_div : levels[li];
		if (mode == BLOCKS) {
			gw = cols * 2; // 2x2 samples per half-block pixel
			gh = rows * 4;
		} else {
			int pxw = ws.ws_xpixel ? ws.ws_xpixel : cols * 8;
			int pxh = ws.ws_ypixel ? ws.ws_ypixel : rows * 16;
			gw = pxw / div > 1 ? (int)(pxw / div) : 1;
			gh = pxh / div > 1 ? (int)(pxh / div) : 1;
		}
		long npx = (long)gw * gh;
		mu = grow(mu, &mu_cap, sizeof *mu * npx);

		double cx = targets[ti].x, cy = targets[ti].y;
		double step = scale / gw;
		int maxit = 150 + (int)(60 * log2(START_SCALE / scale));
		double phase = 0.05 * (frame_start - t0);

		#pragma omp parallel for schedule(dynamic, 1)
		for (int y = 0; y < gh; y++)
			for (int x = 0; x < gw; x++)
				mu[(long)y * gw + x] = escape(cx + (x - gw / 2.0) * step,
				                              cy + (y - gh / 2.0) * step, maxit);

		if (mode == BLOCKS) {
			// Worst case per cell: two 19-byte color escapes plus a 3-byte glyph.
			out = grow(out, &out_cap, (size_t)rows * (cols * 44 + 16) + 64);
			fwrite(out, 1, draw_blocks(out, mu, cols, rows, phase), stdout);
			fflush(stdout);
		} else {
			rgb = grow(rgb, &rgb_cap, 3 * npx);
			#pragma omp parallel for schedule(static)
			for (long i = 0; i < npx; i++) {
				int c[3];
				shade(mu[i], phase, c);
				rgb[3 * i] = c[0];
				rgb[3 * i + 1] = c[1];
				rgb[3 * i + 2] = c[2];
			}

			// Coarsen quickly when frames run long, sharpen slowly when
			// there's headroom.
			double busy = (now() - frame_start) * fps;
			over = busy > 0.85 ? over + 1 : 0;
			under = busy < 0.35 ? under + 1 : 0;
			if (!fixed_div && over >= 5 && li < (int)NLEVELS - 1) {
				li++;
				over = 0;
			} else if (!fixed_div && under >= 30 && li > 0) {
				li--;
				under = 0;
			}

			// Let the terminal finish the previous frame before handing
			// over the next one.
			double wait_start = now();
			while (pending > 0 && !stop && now() - wait_start < 1.0)
				if (read_input(50))
					goto quit;
			pending = 0;

			int slot = ++seq % RING;
			unlink_slot(slot);
			char kind = mode == GFX_SHM ? 's' : 't';
			if (store_pixels(kind, seq, rgb, 3 * npx, ring[slot].name,
			                 sizeof ring[slot].name) == 0) {
				ring[slot].kind = kind;
				char b64[256];
				base64(ring[slot].name, b64);
				int prev = img_id;
				img_id = img_id == 1 ? 2 : 1;
				printf("\x1b[H\x1b_Ga=T,f=24,t=%c,s=%d,v=%d,i=%d,p=1,c=%d,r=%d,C=1;%s\x1b\\",
				       kind, gw, gh, img_id, cols, rows, b64);
				if (prev)
					printf("\x1b_Ga=d,d=I,i=%d,q=2\x1b\\", prev);
				fflush(stdout);
				pending = 1;
			}

			// The terminal accepted the probe but rejects real frames:
			// give up on images.
			if (seq >= 10 && oks == 0) {
				fputs("\x1b_Ga=d,d=A,q=2\x1b\\\x1b[2J", stdout);
				mode = BLOCKS;
				cols = rows = 0;
			}
		}
		frames++;

		scale *= zoom;
		frames_on_target++;
		if (scale < MIN_SCALE ||
		    (frames_on_target > 30 && boring(mu, npx, maxit))) {
			ti = (ti + 1) % NTARGETS;
			scale = START_SCALE;
			frames_on_target = 0;
		}

		// Sleep out the rest of the frame, waking early on a keypress.
		double deadline = frame_start + 1.0 / fps;
		do {
			if (read_input((int)((deadline - now()) * 1000)))
				goto quit;
		} while (!stop && now() < deadline);
	}
quit:
	// Collect the last reply so it isn't left for the shell to read.
	if (mode != BLOCKS) {
		double wait_start = now();
		while (pending > 0 && now() - wait_start < 0.3)
			read_input(50);
	}
	double elapsed = now() - t0;
	restore_term();
	if (verbose) {
		static const char *names[] = { "blocks", "graphics (shared memory)", "graphics (temp file)" };
		fprintf(stderr, "mode: %s", names[mode]);
		if (mode != first_mode)
			fprintf(stderr, " (fell back from %s)", names[first_mode]);
		fprintf(stderr, "\ngrid: %dx%d, %ld frames in %.1fs (%.1f fps)\n",
		        gw, gh, frames, elapsed, frames / elapsed);
		if (first_mode != BLOCKS)
			fprintf(stderr, "image replies: %d ok, %d errors\n", oks, errs);
		if (quit_len) {
			fprintf(stderr, "quit on input:");
			for (int i = 0; i < quit_len; i++)
				fprintf(stderr, " %02x", (unsigned char)quit_bytes[i]);
			fputc('\n', stderr);
		}
	}
	return 0;
}
