////////////////////////////////////////////////////////////////////////////////
// winshot.exe - screenshots wine's screen from INSIDE wine.
//
// Needed because headless gamescope composites into its own buffer: the X
// root is black and every X11 capture lies. This does a BitBlt of the screen
// DC and writes a 24-bit BMP, no dependencies. It is also the sample target
// shipped in the packages (sample\winshot.exe, main at 004012F0).
//
//   winshot.exe file.bmp                    BitBlt of the screen
//   winshot.exe file.bmp "Title" pw         PrintWindow on the window
//   winshot.exe file.bmp "Title" print      WM_PRINT on the window
//   winshot.exe file.bmp "Title" blt        BitBlt of the window rectangle
////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <string.h>

static char  wanted[256];
static char  method[32]="blt";
static HWND  found;

static BOOL CALLBACK Findbytitle(HWND hw,LPARAM lp) {
  char title[512];
  if (!IsWindowVisible(hw))
    return TRUE;
  GetWindowText(hw,title,sizeof(title));
  if (title[0]!='\0' && strncmp(title,wanted,strlen(wanted))==0) {
    found=hw;
    return FALSE;
  };
  return TRUE;
};

static int Savebmp(const char *path,HBITMAP bmp,int dx,int dy,HDC hdc) {
  BITMAPFILEHEADER fh;
  BITMAPINFOHEADER ih;
  int rowsize,imgsize;
  unsigned char *bits;
  FILE *f;

  rowsize=((dx*3+3)/4)*4;
  imgsize=rowsize*dy;
  bits=(unsigned char *)malloc(imgsize);
  if (bits==NULL)
    return 0;

  memset(&ih,0,sizeof(ih));
  ih.biSize=sizeof(ih);
  ih.biWidth=dx;
  ih.biHeight=dy;
  ih.biPlanes=1;
  ih.biBitCount=24;
  ih.biCompression=BI_RGB;
  ih.biSizeImage=imgsize;
  if (GetDIBits(hdc,bmp,0,dy,bits,(BITMAPINFO *)&ih,DIB_RGB_COLORS)==0) {
    free(bits);
    return 0;
  };

  memset(&fh,0,sizeof(fh));
  fh.bfType=0x4D42;
  fh.bfOffBits=sizeof(fh)+sizeof(ih);
  fh.bfSize=fh.bfOffBits+imgsize;

  f=fopen(path,"wb");
  if (f==NULL) {
    free(bits);
    return 0;
  };
  fwrite(&fh,1,sizeof(fh),f);
  fwrite(&ih,1,sizeof(ih),f);
  fwrite(bits,1,imgsize,f);
  fclose(f);
  free(bits);
  return 1;
};

int main(int argc,char *argv[]) {
  HDC hscreen,hmem;
  HBITMAP bmp,oldbmp;
  RECT rc;
  int x,y,dx,dy;
  HWND hw;

  if (argc<2) {
    printf("usage: winshot.exe file.bmp [title [pw|print|blt]]\n");
    return 2;
  };

  hw=NULL;
  if (argc>=4)
    strncpy(method,argv[3],sizeof(method)-1);
  if (argc>=3) {
    strncpy(wanted,argv[2],sizeof(wanted)-1);
    found=NULL;
    EnumWindows(Findbytitle,0);
    hw=found;
    if (hw==NULL) {
      printf("WINDOW NOT FOUND: %s\n",argv[2]);
      return 3;
    };
  };

  if (hw!=NULL) {
    GetWindowRect(hw,&rc);
    x=rc.left; y=rc.top;
    dx=rc.right-rc.left; dy=rc.bottom-rc.top;
  }
  else {
    x=0; y=0;
    dx=GetSystemMetrics(SM_CXSCREEN);
    dy=GetSystemMetrics(SM_CYSCREEN);
  };
  if (dx<=0 || dy<=0) {
    printf("BAD SIZE %dx%d\n",dx,dy);
    return 4;
  };

  hscreen=GetDC(NULL);
  hmem=CreateCompatibleDC(hscreen);
  bmp=CreateCompatibleBitmap(hscreen,dx,dy);
  oldbmp=(HBITMAP)SelectObject(hmem,bmp);
  if (hw!=NULL && strcmp(method,"pw")==0) {
    // PrintWindow asks THE WINDOW to paint itself into our DC: it does not
    // depend on how the screen is composited. 0x02 = PW_RENDERFULLCONTENT.
    printf("PrintWindow=%d ",PrintWindow(hw,hmem,0x02));
  }
  else if (hw!=NULL && strcmp(method,"print")==0) {
    // WM_PRINT: the window repaints into the DC passed. DefWindowProc
    // implements it by calling WM_PAINT, so it works with classic apps.
    SendMessage(hw,WM_PRINT,(WPARAM)hmem,
      PRF_CHILDREN|PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND|PRF_OWNED);
  }
  else {
    BitBlt(hmem,0,0,dx,dy,hscreen,x,y,SRCCOPY);
  };
  SelectObject(hmem,oldbmp);

  if (Savebmp(argv[1],bmp,dx,dy,hscreen)==0) {
    printf("WRITE FAILED\n");
    return 5;
  };
  printf("OK %dx%d -> %s\n",dx,dy,argv[1]);

  DeleteObject(bmp);
  DeleteDC(hmem);
  ReleaseDC(NULL,hscreen);
  return 0;
};
