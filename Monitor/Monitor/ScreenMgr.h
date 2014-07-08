#ifndef __MONITOR_SCREEN_MGR__
#define __MONITOR_SCREEN_MGR__

#include <vector>
#include <thread>
#include <mutex>
#include "Resource.h"
#include "common.h"
#include "Screen.h"

#define MININUM_PADDING 0
#define MININUM_SCREEN_WIDTH  176
#define NUMINUM_SCREEN_HEIGHT 144
#define ROUND(num, fraction) (num) /= (fraction), (num) *= (fraction), (num) / (fraction)
#define GET_FUNC_INDEX(idx) ((idx) < 0 || (idx) >= SCREEN_RES_END ? SCREEN_RES_END : (idx))

typedef enum
{
	SCREEN_RES_1x1,
	SCREEN_RES_2x2,
	SCREEN_RES_1x5,
	SCREEN_RES_3x3,
	SCREEN_RES_END,            // useless
} screen_mgr_res_t;

typedef struct
{
	pj_uint32_t x;
	pj_uint32_t y;
} resolution_t;

using std::mutex;
using std::lock_guard;
using std::vector;

class ScreenMgr;
typedef void (ScreenMgr::*screenmgr_func_t)();

class ScreenMgr
{
public:
	static ScreenMgr *GetInstance();                                         // Singleton
	void Prepare(CWnd *);
	void Prepare(CWnd *, pj_uint32_t, pj_uint32_t, screen_mgr_res_t);
	void Refresh(screen_mgr_res_t);
	void Adjest(pj_int32_t &, pj_int32_t &);
	void HideAll();
	pj_status_t Launch();
	static resolution_t GetDefaultResolution();

private:
	ScreenMgr();
	virtual ~ScreenMgr();

	void Refresh_1x1();
	void Refresh_2x2();
	void Refresh_1x5();
	void Refresh_3x3();

private:
	Screen wall[9];
	const CWnd *wrapper;
	pj_uint32_t last_width, last_height, min_width, min_height;
	screen_mgr_res_t screen_mgr_res;
	pj_uint32_t vertical_padding;
	pj_uint32_t horizontal_padding;
	vector<screenmgr_func_t> refresh_func;
	vector<pj_uint32_t> num_blocks;
	pj_bool_t screen_mgr_active;

	static ScreenMgr *instance;
	static mutex g_instance_mutex;
	static const resolution_t DEFAULT_RESOLUTION;
};

#endif
