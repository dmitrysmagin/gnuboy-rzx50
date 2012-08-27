/*
 * sdl.c
 * sdl interfaces -- based on svga.c
 *
 * (C) 2001 Damian Gryski <dgryski@uwaterloo.ca>
 * Joystick code contributed by David Lau
 * Sound code added by Laguna
 *
 * Licensed under the GPLv2, or later.
 */

#include <stdlib.h>
#include <stdio.h>

#include <linux/soundcard.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <SDL/SDL.h>

#include "bigfontwhite.h"
#include "bigfontred.h"
#include "font.h"
#include "loading.h"
#include "frame.h"

#include "../../fb.h"
#include "../../input.h"
#include "../../rc.h"


struct fb fb;

static int use_yuv = -1;
static int fullscreen = 1;
static int use_altenter = -1;
static int use_joy = -1, sdl_joy_num;
static SDL_Joystick * sdl_joy = NULL;
static const int joy_commit_range = 3276;
static char Xstatus, Ystatus;

static byte *fakescreen = NULL;
static SDL_Surface *screen;
static SDL_Overlay *overlay;
static SDL_Rect overlay_rect;

static SDL_Surface *font = NULL;
static SDL_Surface *bigfontred = NULL;
static SDL_Surface *bigfontwhite = NULL;
static SDL_Surface *loadingscreen = NULL;
static SDL_Surface *frame = NULL;

static bool frameskip = 0;
static bool useframeskip = 0;
static bool showfps = 1;
static int fps = 0;
static int framecounter = 0;
static long time1 = 0;
static int volume = 30;
static int saveslot = 1;

static int vmode[3] = { 0, 0, 16 };
bool startpressed = false;
bool selectpressed = false;
extern bool emuquit;
char * datfile;
static int startvolume=50;

rcvar_t vid_exports[] =
{
	RCV_VECTOR("vmode", &vmode, 3),
	RCV_BOOL("yuv", &use_yuv),
	RCV_BOOL("fullscreen", &fullscreen),
	RCV_BOOL("altenter", &use_altenter),
	RCV_END
};

rcvar_t joy_exports[] =
{
	RCV_BOOL("joy", &use_joy),
	RCV_END
};

/* keymap - mappings of the form { scancode, localcode } - from sdl/keymap.c */
extern int keymap[][2];



#include "../../pcm.h"


struct pcm pcm;

#define DIVIDER 0

static int sound = 1;
static int samplerate = 44100 >> DIVIDER; //44100
static int stereo = 1;
static volatile int audio_done;

rcvar_t pcm_exports[] =
{
	RCV_BOOL("sound", &sound),
	RCV_INT("stereo", &stereo),
	RCV_INT("samplerate", &samplerate),
	RCV_END
};


static int readvolume()
{
	char *mixer_device = "/dev/mixer";
	int mixer;
    int basevolume = 50;
	mixer = open(mixer_device, O_RDONLY);
	if (ioctl(mixer, SOUND_MIXER_READ_VOLUME, &basevolume) == -1) {
		fprintf(stderr, "Failed opening mixer for read - VOLUME\n");
	}
	close(mixer);
	return  (basevolume>>8) & basevolume ;
}

static void setvolume(int involume)
{
	char *mixer_device = "/dev/mixer";
	int mixer;
    int newvolume = involume;
	if (newvolume > 100) newvolume = 100;
	if (newvolume < 0) newvolume = 0;
	int oss_volume = newvolume | (newvolume << 8); // set volume for both channels
	mixer = open(mixer_device, O_WRONLY);
	if (ioctl(mixer, SOUND_MIXER_WRITE_VOLUME, &oss_volume) == -1) {
		fprintf(stderr, "Failed opening mixer for write - VOLUME\n");
	}
	close(mixer);
}

static void audio_callback(void *blah, uint8_t  *stream, int len)
{
   // SDL_LockAudio();
	int teller = 0;
	signed short w;
	byte * bleh = (byte *) stream;
	for (teller = 0; teller < pcm.len; teller++)
	{
		w =  (uint16_t)((pcm.buf[teller] - 128) << 8);
		*bleh++ = w & 0xFF ;
		*bleh++ = w >> 8;
    //  from rlyeh
    // ((signed short *)stream)[teller ] = ( pcm.buf[ teller ] ^ 0x80 ) << 8;

	}
	audio_done = 1;
	//SDL_UnlockAudio();
}


void pcm_init()
{
	int i;
	SDL_AudioSpec as;

	if (!sound) return;

	as.freq = samplerate;
	as.format = AUDIO_S16;
	as.channels = 1 + stereo;
	as.samples = (2048 >> (DIVIDER - stereo + 1));
	as.callback = audio_callback;
	as.userdata = 0;
	if (SDL_OpenAudio(&as, 0) == -1)
		return;

	pcm.hz = as.freq;
	pcm.stereo = as.channels - 1;
	//printf("%d\n",as.size);
	pcm.len = as.size >> 1;
	pcm.buf = malloc(pcm.len);
	pcm.pos = 0;
	memset(pcm.buf, 0, pcm.len);

	SDL_PauseAudio(0);
}

int pcm_submit()
{
	if (!pcm.buf) return 0;
	if (pcm.pos < pcm.len) return 1;
	while (!audio_done)
		SDL_Delay(1);
	audio_done = 0;
	pcm.pos = 0;
	return 1;
}

void pcm_close()
{
	if (sound)
        SDL_CloseAudio();
}

/* alekmaul's scaler taken from mame4all */
void bitmap_scale(int startx, int starty, int viswidth, int visheight, int newwidth, int newheight,int pitch, uint16_t *src, uint16_t *dst) {
  unsigned int W,H,ix,iy,x,y;
  x=startx<<16;
  y=starty<<16;
  W=newwidth;
  H=newheight;
  ix=(viswidth<<16)/W;
  iy=(visheight<<16)/H;

  do
  {
    uint16_t *buffer_mem=&src[(y>>16)*320];
    W=newwidth; x=startx<<16;
    do {
      *dst++=buffer_mem[x>>16];
      x+=ix;
    } while (--W);
    dst+=pitch;
    y+=iy;
  } while (--H);
}

/* end alekmaul's scaler taken from mame4all */

uint16_t gfx_font_width(SDL_Surface* inFont, char* inString) {
	if((inFont == NULL) || (inString == NULL))
		return 0;
	uintptr_t i, tempCur, tempMax;
	for(i = 0, tempCur = 0, tempMax = 0; inString[i] != '\0'; i++) {
		if(inString[i] == '\t')
			tempCur += 4;
		else if((inString[i] == '\r') || (inString[i] == '\n'))
			tempCur = 0;
		else
			tempCur++;
		if(tempCur > tempMax) tempMax = tempCur;
	}
	tempMax *= (inFont->w >> 4);
	return tempMax;
}

uint16_t gfx_font_height(SDL_Surface* inFont) {
	if(inFont == NULL)
		return 0;
	return (inFont->h >> 4);
}


void gfx_font_print(int16_t inX, int16_t inY, SDL_Surface* inFont, char* inString) {
	if((inFont == NULL) || (inString == NULL))
		return;

	uint16_t* tempBuffer = screen->pixels;
	uint16_t* tempFont = inFont->pixels;
	uint8_t*  tempChar;
	int16_t   tempX = inX;
	int16_t   tempY = inY;
	uintptr_t i, j, x, y;

    SDL_LockSurface(screen);
    SDL_LockSurface(inFont);

	for(tempChar = (uint8_t*)inString; *tempChar != '\0'; tempChar++) {
		if(*tempChar == ' ') {
			tempX += (inFont->w >> 4);
			continue;
		}
		if(*tempChar == '\t') {
			tempX += ((inFont->w >> 4) << 2);
			continue;
		}
		if(*tempChar == '\r') {
			tempX = inX;
			continue;
		}
		if(*tempChar == '\n') {
			tempX = inX;
			tempY += (inFont->h >> 4);
			continue;
		}
		for(j = ((*tempChar >> 4) * (inFont->h >> 4)), y = tempY; (j < (((*tempChar >> 4) + 1) * (inFont->h >> 4))) && (y < 240); j++, y++) {
			for(i = ((*tempChar & 0x0F) * (inFont->w >> 4)), x = tempX; (i < (((*tempChar & 0x0F) + 1) * (inFont->w >> 4))) && (x < 320); i++, x++) {
				tempBuffer[(y * 320) + x] |= tempFont[(j * inFont->w) + i];
			}
		}
		tempX += (inFont->w >> 4);
	}
    SDL_UnlockSurface(screen);
    SDL_UnlockSurface(inFont);
}


void gfx_font_print_char(int16_t inX, int16_t inY, SDL_Surface* inFont, char inChar) {
	char tempStr[2] = { inChar, '\0' };
	gfx_font_print(inX, inY, inFont, tempStr);
}

void gfx_font_print_center(int16_t inY, SDL_Surface* inFont, char* inString) {
	int16_t tempX = (320 - gfx_font_width(inFont, inString)) >> 1;
	gfx_font_print(tempX, inY, inFont, inString);
}

void gfx_font_print_fromright(int16_t inX, int16_t inY, SDL_Surface* inFont, char* inString) {
	int16_t tempX = inX - gfx_font_width(inFont, inString);
	gfx_font_print(tempX, inY, inFont, inString);
}


SDL_Surface* gfx_tex_load_tga(const char* inPath) {
	if(inPath == NULL)
		return NULL;

	FILE* tempFile = fopen(inPath, "rb");
	if(tempFile == NULL)
		return NULL;

	uint8_t  tga_ident_size;
	uint8_t  tga_color_map_type;
	uint8_t  tga_image_type;
	uint16_t tga_color_map_start;
	uint16_t tga_color_map_length;
	uint8_t  tga_color_map_bpp;
	uint16_t tga_origin_x;
	uint16_t tga_origin_y;
	uint16_t tga_width;
	uint16_t tga_height;
	uint8_t  tga_bpp;
	uint8_t  tga_descriptor;

	fread(&tga_ident_size, 1, 1, tempFile);
	fread(&tga_color_map_type, 1, 1, tempFile);
	fread(&tga_image_type, 1, 1, tempFile);
	fread(&tga_color_map_start, 2, 1, tempFile);
	fread(&tga_color_map_length, 2, 1, tempFile);
	fread(&tga_color_map_bpp, 1, 1, tempFile);
	fread(&tga_origin_x, 2, 1, tempFile);
	fread(&tga_origin_y, 2, 1, tempFile);
	fread(&tga_width, 2, 1, tempFile);
	fread(&tga_height, 2, 1, tempFile);
	fread(&tga_bpp, 1, 1, tempFile);
	fread(&tga_descriptor, 1, 1, tempFile);

	SDL_Surface* tempTexture = SDL_CreateRGBSurface(SDL_SWSURFACE, tga_width, tga_height, 16, screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, screen->format->Amask);
	if(tempTexture == NULL) {
		fclose(tempFile);
		return NULL;
	}
	bool upsideDown = (tga_descriptor & 0x20) > 0;

	uintptr_t i;
	uintptr_t iNew;
	uint8_t tempColor[3];
	SDL_LockSurface(tempTexture);
	uint16_t* tempTexPtr = tempTexture->pixels;
	for(i = 0; i < (tga_width * tga_height); i++) {
		fread(&tempColor[2], 1, 1, tempFile);
		fread(&tempColor[1], 1, 1, tempFile);
		fread(&tempColor[0], 1, 1, tempFile);

		if (upsideDown)
			iNew = i;
		else
			iNew = (tga_height - 1 - (i / tga_width)) * tga_width + i % tga_width;

		tempTexPtr[iNew] = SDL_MapRGB(screen->format,tempColor[0], tempColor[1], tempColor[2]);
	}
	SDL_UnlockSurface(tempTexture);
	fclose(tempFile);

	return tempTexture;
}

SDL_Surface* gfx_tex_load_tga_from_array(uint8_t* buffer) {
	if(buffer == NULL)
		return NULL;

	uint8_t  tga_ident_size;
	uint8_t  tga_color_map_type;
	uint8_t  tga_image_type;
	uint16_t tga_color_map_start;
	uint16_t tga_color_map_length;
	uint8_t  tga_color_map_bpp;
	uint16_t tga_origin_x;
	uint16_t tga_origin_y;
	uint16_t tga_width;
	uint16_t tga_height;
	uint8_t  tga_bpp;
	uint8_t  tga_descriptor;

	tga_ident_size = buffer[0];
	tga_color_map_type = buffer[1];
	tga_image_type = buffer[2];
	tga_color_map_start = buffer[3] + (buffer[4] << 8);
	tga_color_map_length = buffer[5] + (buffer[6] << 8);
	tga_color_map_bpp = buffer[7];
	tga_origin_x = buffer[8] + (buffer[9] << 8);
	tga_origin_y = buffer[10] + (buffer[11] << 8);
	tga_width = buffer[12] + (buffer[13] << 8);
	tga_height = buffer[14] + (buffer[15] << 8);
	tga_bpp = buffer[16];
	tga_descriptor = buffer[17];

	uint32_t bufIndex = 18;

	SDL_Surface* tempTexture = SDL_CreateRGBSurface(SDL_SWSURFACE, tga_width, tga_height, 16, screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, screen->format->Amask);
	if(tempTexture == NULL) {
		return NULL;
	}

	bool upsideDown = (tga_descriptor & 0x20) > 0;

	uintptr_t i;
	uintptr_t iNew;
	uint8_t tempColor[3];
    SDL_LockSurface(tempTexture);
	uint16_t* tempTexPtr = tempTexture->pixels;
	for(i = 0; i < (tga_width * tga_height); i++) {
		tempColor[2] = buffer[bufIndex + 0];
		tempColor[1] = buffer[bufIndex + 1];
		tempColor[0] = buffer[bufIndex + 2];
		bufIndex += 3;

		if (upsideDown)
			iNew = i;
		else
			iNew = (tga_height - 1 - (i / tga_width)) * tga_width + i % tga_width;

		tempTexPtr[iNew] = SDL_MapRGB(screen->format,tempColor[0], tempColor[1], tempColor[2]);
	}
    SDL_UnlockSurface(tempTexture);
	return tempTexture;
}

void menu()
{
    bool pressed = false;
    int currentselection = 1;
    int miniscreenwidth = 140;
    int miniscreenheight = 135;
    SDL_Rect dstRect;
    char *cmd = NULL;
    SDL_Event Event;
    SDL_Surface* miniscreen = SDL_CreateRGBSurface(SDL_SWSURFACE, miniscreenwidth, miniscreenheight, 16, screen->format->Rmask, screen->format->Gmask, screen->format->Bmask, screen->format->Amask);
    SDL_LockSurface(miniscreen);
    bitmap_scale(80,48,160,144,miniscreenwidth,miniscreenheight,0,(uint16_t*)fakescreen,(uint16_t*)miniscreen->pixels);
    SDL_UnlockSurface(miniscreen);
    char text[50];
    SDL_PollEvent(&Event);
    while (((currentselection != 1) && (currentselection != 8)) || (!pressed))
    {

        pressed = false;
        SDL_FillRect( screen, NULL, 0 );

        dstRect.x = 320-5-miniscreenwidth;
        dstRect.y = 30;
        dstRect.h = miniscreenheight;
        dstRect.w = miniscreenwidth;
        SDL_BlitSurface(miniscreen,NULL,screen,&dstRect);

        gfx_font_print_center(5,bigfontwhite,"DINGUX GNUBOY");

        if (currentselection == 1)
            gfx_font_print(5,25,bigfontred,"Continue");
        else
            gfx_font_print(5,25,bigfontwhite,"Continue");


        sprintf(text,"Volume %d",volume);

        if (currentselection == 2)
            gfx_font_print(5,45,bigfontred,text);
        else
            gfx_font_print(5,45,bigfontwhite,text);

        sprintf(text,"%s",useframeskip ? "Frameskip on" : "Frameskip off");

        if (currentselection == 3)
            gfx_font_print(5,65,bigfontred,text);
        else
            gfx_font_print(5,65,bigfontwhite,text);

        sprintf(text,"Save State %d",saveslot);

        if (currentselection == 4)
            gfx_font_print(5,85,bigfontred,text);
        else
            gfx_font_print(5,85,bigfontwhite,text);

        sprintf(text,"Load State %d",saveslot);

        if (currentselection == 5)
            gfx_font_print(5,105,bigfontred,text);
        else
            gfx_font_print(5,105,bigfontwhite,text);

        if (currentselection == 6)
        {
            if (fullscreen == 1)
                gfx_font_print(5,125,bigfontred,"Stretched FS");
            else
                if (fullscreen == 2)
                    gfx_font_print(5,125,bigfontred,"Aspect FS");
                else
                    gfx_font_print(5,125,bigfontred,"Native Res");
        }
        else
        {
            if (fullscreen == 1)
                gfx_font_print(5,125,bigfontwhite,"Stretched FS");
            else
                if (fullscreen == 2)
                    gfx_font_print(5,125,bigfontwhite,"Aspect FS");
                else
                    gfx_font_print(5,125,bigfontwhite,"Native Res");
        }

        sprintf(text,"%s",showfps ? "Show fps on" : "Show fps off");

        if (currentselection == 7)
            gfx_font_print(5,145,bigfontred,text);
        else
            gfx_font_print(5,145,bigfontwhite,text);


        if (currentselection == 8)
            gfx_font_print(5,165,bigfontred,"Quit");
        else
            gfx_font_print(5,165,bigfontwhite,"Quit");









        gfx_font_print_center(240-40-gfx_font_height(font),font,"Dingux GnuBoy has been ported by joyrider");
        gfx_font_print_center(240-30-gfx_font_height(font),font,"Thanks to alekmaul for the scaler and sound example,");
        gfx_font_print_center(240-20-gfx_font_height(font),font,"Flatmush for his awesome minilib,");
        gfx_font_print_center(240-10-gfx_font_height(font),font,"Harteex for testing and the tga loading,");
        gfx_font_print_center(240-gfx_font_height(font),font,"Alf for the beautiful gameboy frames !");

        while (SDL_PollEvent(&Event))
        {
            if (Event.type == SDL_KEYDOWN)
            {
                switch(Event.key.keysym.sym)
                {
                    case SDLK_UP:
                        currentselection--;
                        if (currentselection == 0)
                            currentselection = 8;
                        break;
                    case SDLK_DOWN:
                        currentselection++;
                        if (currentselection == 9)
                            currentselection = 1;
                        break;
                    case SDLK_LCTRL:
                    case SDLK_LALT:
                    case SDLK_RETURN:
                        pressed = true;
                        break;
                    case SDLK_TAB:
                        if(currentselection == 2)
                        {
                            volume-=10;
                            if (volume < 0)
                                volume = 0;
                            setvolume(volume);
                        }
                        break;
                    case SDLK_BACKSPACE:
                         if(currentselection == 2)
                        {
                            volume+=10;
                            if (volume > 100)
                                volume = 100;
                            setvolume(volume);
                        }
                        break;
                    case SDLK_LEFT:
                        switch(currentselection)
                        {
                            case 2:
                                volume--;
                                if (volume < 0)
                                    volume = 0;
                                setvolume(volume);
                                break;
                            case 3:
                                useframeskip = !useframeskip;
                                break;
                            case 4:
                            case 5:
                                saveslot--;
                                if (saveslot < 1)
                                    saveslot = 1;
                                cmd = malloc(strlen("set saveslot X") +1);
                                sprintf(cmd,"%s%d","set saveslot ",saveslot);
                                rc_command(cmd);
                                free(cmd);
                                break;
                            case 6:
                                fullscreen--;
                                    if (fullscreen < 0)
                                        fullscreen = 2;
                                break;
                            case 7:
                                showfps = !showfps;
                                break;

                        }
                        break;
                    case SDLK_RIGHT:
                        switch(currentselection)
                        {
                            case 2:
                                volume++;
                                if (volume >100)
                                    volume = 100;
                                setvolume(volume);
                                break;
                            case 3:
                                useframeskip = !useframeskip;
                                break;
                            case 4:
                            case 5:
                                saveslot++;
                                if (saveslot > 9)
                                    saveslot = 9;
                                cmd = malloc(strlen("set saveslot X") +1);
                                sprintf(cmd,"%s%d","set saveslot ",saveslot);
                                rc_command(cmd);
                                free(cmd);
                                break;
                            case 6:
                                fullscreen++;
                                if (fullscreen > 2)
                                    fullscreen = 0;
                                break;
                            case 7:
                                showfps = !showfps;
                                break;
                        }
                        break;


                }
            }
        }

        if (pressed)
        {
            switch(currentselection)
            {
                case 3:
                    useframeskip = !useframeskip;
                    break;
                case 7:
                    showfps = !showfps;
                    break;
                case 6 :
                    fullscreen++;
                    if (fullscreen > 2)
                        fullscreen = 0;
                    break;
                case 4 :
                	cmd = malloc(strlen("savestate") +1);
                    sprintf(cmd,"%s","savestate");
                    rc_command(cmd);
                    free(cmd);
                    currentselection = 1;
                    break;
                case 5 :
                    cmd = malloc(strlen("loadstate") +1);
                    sprintf(cmd,"%s","loadstate");
                    rc_command(cmd);
                    free(cmd);
                    currentselection = 1;
                    break;
            }
        }

        SDL_Flip(screen);
        SDL_Delay(4);
    }
    if (currentselection == 8)
    {
        emuquit = true;
    }
    free(miniscreen);
}


static int mapscancode(SDLKey sym)
{
	/* this could be faster:  */
	/*  build keymap as int keymap[256], then ``return keymap[sym]'' */

	int i;
	for (i = 0; keymap[i][0]; i++)
		if (keymap[i][0] == sym)
			return keymap[i][1];
	if (sym >= '0' && sym <= '9')
		return sym;
	if (sym >= 'a' && sym <= 'z')
		return sym;
	return 0;
}


static void joy_init()
{
	int i;
	int joy_count;

	/* Initilize the Joystick, and disable all later joystick code if an error occured */
	if (!use_joy) return;

	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK))
		return;

	joy_count = SDL_NumJoysticks();

	if (!joy_count)
		return;

	/* now try and open one. If, for some reason it fails, move on to the next one */
	for (i = 0; i < joy_count; i++)
	{
		sdl_joy = SDL_JoystickOpen(i);
		if (sdl_joy)
		{
			sdl_joy_num = i;
			break;
		}
	}

	/* make sure that Joystick event polling is a go */
	SDL_JoystickEventState(SDL_ENABLE);
}

static void overlay_init()
{
	if (!use_yuv) return;

	if (use_yuv < 0)
		if (vmode[0] < 320 || vmode[1] < 288)
			return;

	overlay = SDL_CreateYUVOverlay(320, 144, SDL_YUY2_OVERLAY, screen);

	if (!overlay) return;

	if (!overlay->hw_overlay || overlay->planes > 1)
	{
		SDL_FreeYUVOverlay(overlay);
		overlay = 0;
		return;
	}

	SDL_LockYUVOverlay(overlay);

	fb.w = 160;
	fb.h = 144;
	fb.pelsize = 4;
	fb.pitch = overlay->pitches[0];
	fb.ptr = overlay->pixels[0];
	fb.yuv = 1;
	fb.cc[0].r = fb.cc[1].r = fb.cc[2].r = fb.cc[3].r = 0;
	fb.dirty = 1;
	fb.enabled = 1;

	overlay_rect.x = 0;
	overlay_rect.y = 0;
	overlay_rect.w = vmode[0];
	overlay_rect.h = vmode[1];

	/* Color channels are 0=Y, 1=U, 2=V, 3=Y1 */
	switch (overlay->format)
	{
		/* FIXME - support more formats */
	case SDL_YUY2_OVERLAY:
	default:
		fb.cc[0].l = 0;
		fb.cc[1].l = 24;
		fb.cc[2].l = 8;
		fb.cc[3].l = 16;
		break;
	}

	SDL_UnlockYUVOverlay(overlay);
}

void vid_init(char *s, char *s2)
{
	int flags;

	flags = SDL_SWSURFACE;
	if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO))
		die("SDL: Couldn't initialize SDL: %s\n", SDL_GetError());
	if (!(screen = SDL_SetVideoMode(320, 240, 16, flags)))
		die("SDL: can't set video mode: %s\n", SDL_GetError());

	SDL_ShowCursor(0);
//	joy_init();

//	overlay_init();

    loadingscreen = gfx_tex_load_tga_from_array(loading);
    SDL_BlitSurface(loadingscreen,NULL,screen,NULL);
    SDL_Flip(screen);

    frame = gfx_tex_load_tga(s);
    if (!frame)
        frame = gfx_tex_load_tga(s2);
    if (!frame)
        frame = gfx_tex_load_tga("./gnuboy.tga");
    if (!frame)

    frame = gfx_tex_load_tga_from_array(framearray);

    font = gfx_tex_load_tga_from_array(fontarray);
    bigfontwhite = gfx_tex_load_tga_from_array(bigfontwhitearray);
    bigfontred = gfx_tex_load_tga_from_array(bigfontredarray);


//	if (fb.yuv) return;

	fakescreen = calloc(320*240, 2);


	fb.w = 320;
	fb.h = 240;
	fb.ptr = fakescreen;
	fb.pelsize = 2;
	fb.pitch = 640;
	fb.indexed = 0;
	fb.cc[0].r = 3;
	fb.cc[1].r = 2;
	fb.cc[2].r = 3;
	fb.cc[0].l = 11;
	fb.cc[1].l = 5;
	fb.cc[2].l = 0;

    if(fullscreen == 0)
        SDL_BlitSurface(frame,NULL,screen,NULL);
    else
        if(fullscreen == 2)
             SDL_FillRect(screen,NULL,0);


    SDL_Flip(screen);

	fb.enabled = 1;
	fb.dirty = 0;
}


void ev_poll()
{
	event_t ev;
	SDL_Event event;
	int axisval;

	while (SDL_PollEvent(&event))
	{
		switch(event.type)
		{
		case SDL_ACTIVEEVENT:
			if (event.active.state == SDL_APPACTIVE)
				fb.enabled = event.active.gain;
			break;
		case SDL_KEYDOWN:
			if (event.key.keysym.sym == SDLK_RETURN)
			{
                startpressed = true;
			}

            if (event.key.keysym.sym == SDLK_ESCAPE)
            {
                selectpressed = true;
            }
			ev.type = EV_PRESS;
			ev.code = mapscancode(event.key.keysym.sym);
			ev_postevent(&ev);
			break;
		case SDL_KEYUP:
            if (event.key.keysym.sym == SDLK_RETURN)
            {
                startpressed = false;
            }
            if (event.key.keysym.sym == SDLK_ESCAPE)
            {
                selectpressed = false;
            }
			ev.type = EV_RELEASE;
			ev.code = mapscancode(event.key.keysym.sym);
			ev_postevent(&ev);
			break;
//		case SDL_JOYAXISMOTION:
//			switch (event.jaxis.axis)
//			{
//			case 0: /* X axis */
//				axisval = event.jaxis.value;
//				if (axisval > joy_commit_range)
//				{
//					if (Xstatus==2) break;
//
//					if (Xstatus==0)
//					{
//						ev.type = EV_RELEASE;
//						ev.code = K_JOYLEFT;
//        			  		ev_postevent(&ev);
//					}
//
//					ev.type = EV_PRESS;
//					ev.code = K_JOYRIGHT;
//					ev_postevent(&ev);
//					Xstatus=2;
//					break;
//				}
//
//				if (axisval < -(joy_commit_range))
//				{
//					if (Xstatus==0) break;
//
//					if (Xstatus==2)
//					{
//						ev.type = EV_RELEASE;
//						ev.code = K_JOYRIGHT;
//        			  		ev_postevent(&ev);
//					}
//
//					ev.type = EV_PRESS;
//					ev.code = K_JOYLEFT;
//					ev_postevent(&ev);
//					Xstatus=0;
//					break;
//				}
//
//				/* if control reaches here, the axis is centered,
//				 * so just send a release signal if necisary */
//
//				if (Xstatus==2)
//				{
//					ev.type = EV_RELEASE;
//					ev.code = K_JOYRIGHT;
//					ev_postevent(&ev);
//				}
//
//				if (Xstatus==0)
//				{
//					ev.type = EV_RELEASE;
//					ev.code = K_JOYLEFT;
//					ev_postevent(&ev);
//				}
//				Xstatus=1;
//				break;
//
//			case 1: /* Y axis*/
//				axisval = event.jaxis.value;
//				if (axisval > joy_commit_range)
//				{
//					if (Ystatus==2) break;
//
//					if (Ystatus==0)
//					{
//						ev.type = EV_RELEASE;
//						ev.code = K_JOYUP;
//        			  		ev_postevent(&ev);
//					}
//
//					ev.type = EV_PRESS;
//					ev.code = K_JOYDOWN;
//					ev_postevent(&ev);
//					Ystatus=2;
//					break;
//				}
//
//				if (axisval < -joy_commit_range)
//				{
//					if (Ystatus==0) break;
//
//					if (Ystatus==2)
//					{
//						ev.type = EV_RELEASE;
//						ev.code = K_JOYDOWN;
//        			  		ev_postevent(&ev);
//					}
//
//					ev.type = EV_PRESS;
//					ev.code = K_JOYUP;
//					ev_postevent(&ev);
//					Ystatus=0;
//					break;
//				}
//
//				/* if control reaches here, the axis is centered,
//				 * so just send a release signal if necisary */
//
//				if (Ystatus==2)
//				{
//					ev.type = EV_RELEASE;
//					ev.code = K_JOYDOWN;
//					ev_postevent(&ev);
//				}
//
//				if (Ystatus==0)
//				{
//					ev.type = EV_RELEASE;
//					ev.code = K_JOYUP;
//					ev_postevent(&ev);
//				}
//				Ystatus=1;
//				break;
//			}
//			break;
//		case SDL_JOYBUTTONUP:
//			if (event.jbutton.button>15) break;
//			ev.type = EV_RELEASE;
//			ev.code = K_JOY0 + event.jbutton.button;
//			ev_postevent(&ev);
//			break;
//		case SDL_JOYBUTTONDOWN:
//			if (event.jbutton.button>15) break;
//			ev.type = EV_PRESS;
//			ev.code = K_JOY0+event.jbutton.button;
//			ev_postevent(&ev);
//			break;
		case SDL_QUIT:
			exit(1);
			break;
		default:
			break;
		}
	}

    if(startpressed && selectpressed)
    {
        //a320_sound_thread_mute();
        //a320_sound_paused = 1;
        event_t ev;
        ev.type = EV_RELEASE;
        ev.code = mapscancode(SDLK_RETURN);
        ev_postevent(&ev);
        ev.type = EV_RELEASE;
        ev.code = mapscancode(SDLK_ESCAPE);
        ev_postevent(&ev);
        memset(pcm.buf, 0, pcm.len);
        menu();
        startpressed = false;
        selectpressed = false;
        if(emuquit)
            memset(pcm.buf, 0, pcm.len);
        if(fullscreen == 0)
            SDL_BlitSurface(frame,NULL,screen,NULL);
        else
            SDL_FillRect(screen, NULL, 0 );

        SDL_Flip(screen);

    }

}

void vid_setpal(int i, int r, int g, int b)
{
//	SDL_Color col;

//	col.r = r; col.g = g; col.b = b;

//	SDL_SetColors(screen, &col, i, 1);
}

void vid_preinit(char *s)
{

    FILE *f;
    char *cmd;
    int vol;
    startvolume = readvolume();
    datfile = strdup(s);
    f = fopen(datfile,"rb");
    if(f)
    {
        fread(&fullscreen,sizeof(int),1,f);
        fread(&volume,sizeof(int),1,f);
        fread(&saveslot,sizeof(int),1,f);
        fread(&useframeskip,sizeof(bool),1,f);
        fread(&showfps,sizeof(bool),1,f);
        fclose(f);
    }
    else
    {
        fullscreen = 1;
        volume = 75;
        saveslot = 1;
        useframeskip = false;
    }
    cmd = malloc(strlen("set saveslot X") +1);
    sprintf(cmd,"%s%d","set saveslot ",saveslot);
    rc_command(cmd);
    free(cmd);
    setvolume(volume);
}

void vid_close()
{
    setvolume(startvolume);
	if(fakescreen);
        free(fakescreen);

	if(font)
        SDL_FreeSurface(font);

    if(bigfontred)
        SDL_FreeSurface(bigfontred);

    if(bigfontwhite);
        SDL_FreeSurface(bigfontwhite);

	if(loadingscreen)
        SDL_FreeSurface(loadingscreen);

	if(frame)
        SDL_FreeSurface(frame);

    if(screen);
	{
        SDL_UnlockSurface(screen);
        SDL_FreeSurface(screen);
        SDL_Quit();

        FILE *f;
        printf("%s\n",datfile);
        f = fopen(datfile,"wb");
        if(f)
        {
            fwrite(&fullscreen,sizeof(int),1,f);
            fwrite(&volume,sizeof(int),1,f);
            fwrite(&saveslot,sizeof(int),1,f);
            fwrite(&useframeskip,sizeof(bool),1,f);
            fwrite(&showfps,sizeof(bool),1,f);
            fsync(f);
            fclose(f);
        }
    }
	fb.enabled = 0;
}

void vid_settitle(char *title)
{
	SDL_WM_SetCaption(title, title);
}


void vid_begin()
{
    SDL_Rect dest;
    char Text[100];
    if (!emuquit)
    {
        if ((!frameskip) ||  (!useframeskip))
        {
            SDL_LockSurface(screen);
            //fb.ptr = screen->pixels;
           if (fullscreen == 1)
                    bitmap_scale(80,48,160,144,320,240,0,(uint16_t*)fakescreen,(uint16_t*)screen->pixels);
                else
                    if (fullscreen == 2)
                        bitmap_scale(80,48,160,144,267,240,53,(uint16_t*)fakescreen,(uint16_t*)screen->pixels+25);
                    else
                        bitmap_scale(80,48,160,144,160,144,160,(uint16_t*)fakescreen,(uint16_t*)screen->pixels+15440);


            if(showfps)
            {
                sprintf(Text,"%d",fps);
                dest.x = 0;
                dest.y = 0;
                dest.w = gfx_font_width(font,Text);
                dest.h = gfx_font_height(font);
                SDL_FillRect(screen,&dest,0);
                gfx_font_print(0,0,font,Text);
                framecounter++;
                if (SDL_GetTicks() - time1 > 1000)
                {
                    fps=framecounter;
                    framecounter=0;
                    time1 = SDL_GetTicks();
                }
            }

            SDL_UnlockSurface(screen);
            SDL_Flip(screen);

        }
        frameskip = !frameskip;
    }
}

void vid_end()
{
//	if (overlay)
//	{
//		SDL_UnlockYUVOverlay(overlay);
//		if (fb.enabled)
//			SDL_DisplayYUVOverlay(overlay, &overlay_rect);
//		return;
//	}
//	bitmap_scale(80,48,160,144,320,240,0,(uint16_t*)fakescreen,(uint16_t*)screen->pixels);
//	SDL_UnlockSurface(screen);
//
//	if (fb.enabled) SDL_Flip(screen);
}









