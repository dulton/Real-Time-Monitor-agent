#include "stdafx.h"
#include "SessionMgr.h"

pj_bool_t SessionMgr::g_call_report = PJ_TRUE;
pj_oshandle_t SessionMgr::g_log_handle;
mutex SessionMgr::g_instance_mutex;
pj_bool_t SessionMgr::g_quit_flag = PJ_FALSE;
pj_str_t SessionMgr::POOL_NAME = pj_str("monitor_pool");
pj_str_t SessionMgr::LOG_NAME = pj_str("monitor.log");
SessionMgr *SessionMgr::g_instance =  new SessionMgr();
pjsip_module SessionMgr::g_monitor_module = 
{
	NULL, NULL,				                 /* prev, next.		 */
    { "monitor_module", 13 },	     	     /* Name.		     */
    -1,					                     /* Id			     */
    PJSIP_MOD_PRIORITY_APPLICATION,          /* Priority	     */
    NULL,									 /* load()		     */
    NULL,									 /* start()		     */
    NULL,									 /* stop()		     */
    NULL,									 /* unload()		 */
    NULL,									 /* on_rx_request()	 */
    NULL,									 /* on_rx_response() */
    NULL,									 /* on_tx_request.	 */
    NULL,									 /* on_tx_response() */
    NULL,									 /* on_tsx_state()	 */
};
pjsip_module SessionMgr::g_logger_module = 
{
	NULL, NULL,				                 /* prev, next.		 */
    { "logger_module", 14 },	     	     /* Name.		     */
    -1,					                     /* Id			     */
    PJSIP_MOD_PRIORITY_TRANSPORT_LAYER - 1,  /* Priority	     */
    NULL,									 /* load()		     */
    NULL,									 /* start()		     */
    NULL,									 /* stop()		     */
    NULL,									 /* unload()		 */
    &SessionMgr::LogRxDelivery,				 /* on_rx_request()	 */
    &SessionMgr::LogRxDelivery,				 /* on_rx_response() */
    &SessionMgr::LogTxDelivery,				 /* on_tx_request.	 */
    &SessionMgr::LogTxDelivery,				 /* on_tx_response() */
    NULL,									 /* on_tsx_state()	 */
};

pj_bool_t SessionMgr::LogRxDelivery(pjsip_rx_data *rdata)
{
	/* Always return false, otherwise messages will not get processed! */
	return PJ_FALSE;
}

pj_status_t SessionMgr::LogTxDelivery(pjsip_tx_data *tdata)
{
	/* Always return success, otherwise message will not get sent! */
    return PJ_SUCCESS;
}

void SessionMgr::LogPerform(int level, const char *log, int loglen)
{
	pj_log_write(level, log, loglen);

	pj_ssize_t log_size = loglen;
	pj_file_write(g_log_handle, log, &log_size);
	pj_file_flush(g_log_handle);
}

SessionMgr *SessionMgr::GetInstance()
{
	lock_guard<mutex> internal_lock(g_instance_mutex);
	pj_assert(g_instance);
	return g_instance;
}

SessionMgr::SessionMgr()
	: sip_thread_cnt_(MAXIMAL_THREAD_NUM)
	, caching_pool_()
	, pool_(NULL)
	, sip_endpt_(NULL)
	, media_endpt_(NULL)
	, local_sip_addr_()
	, local_sip_uri_()
	, local_sip_port_(0)
	, local_media_port_(0)
	, sessions_(MAXIMAL_SCREEN_NUM)
	, sip_threads_(sip_thread_cnt_)
{
	for (pj_uint32_t idx = 0; idx < MAXIMAL_SCREEN_NUM; ++ idx)
	{
		sessions()[idx] = new Session(idx);
	}
}

SessionMgr::~SessionMgr()
{
}

pj_status_t SessionMgr::Prepare(pj_str_t local_sip_addr, pj_uint16_t local_sip_port, pj_uint16_t media_start_port)
{
	pj_status_t status;

	set_local_sip_port(local_sip_port);
	set_local_sip_addr(local_sip_addr);
	set_local_media_port(media_start_port);

	static char local_uri[64];
    pj_ansi_sprintf(local_uri, "sip:%s:%d", local_sip_addr.ptr, local_sip_port);
	pj_str_t str_local_uri = pj_str(local_uri);
    set_local_sip_uri(str_local_uri);

	pj_log_set_log_func( &LogPerform );

    /* init PJLIB-UTIL: */
    status = pjlib_util_init();
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	pj_caching_pool_init(&caching_pool(), &pj_pool_factory_default_policy, 0);

	set_pool(pj_pool_create(&caching_pool().factory, POOL_NAME.ptr, 1000, 1000, NULL));

	pj_file_open(pool(), LOG_NAME.ptr, PJ_O_WRONLY | PJ_O_APPEND, &g_log_handle);

	status = pjsip_endpt_create(&caching_pool().factory, pj_gethostname()->ptr, &sip_endpt_);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	{
		pj_sockaddr_in addr;
		pjsip_transport *tp;

		pj_bzero(&addr, sizeof(addr));
		addr.sin_family = pj_AF_INET();
		addr.sin_addr.s_addr = 0;
		addr.sin_port = pj_htons((pj_uint16_t)local_sip_port);

		status = pjsip_udp_transport_start(sip_endpt(), &addr, NULL, 1, &tp);
		if (status != PJ_SUCCESS)
		{
			return status;
		}

		PJ_LOG(3,(THIS_FILE, "SIP UDP listening on %.*s:%d",
			  (int)tp->local_name.host.slen, tp->local_name.host.ptr,
			  tp->local_name.port));
	}

	status = pjsip_tsx_layer_init_module(sip_endpt());
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /*  Initialize UA layer. */
    status = pjsip_ua_init_module(sip_endpt(), NULL);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* Initialize 100rel support */
    status = pjsip_100rel_init_module(sip_endpt());
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	{
		pjsip_inv_callback inv_cb;

		/* Init the callback for INVITE session: */
		pj_bzero(&inv_cb, sizeof(inv_cb));
		inv_cb.on_state_changed = &OnStateChanged;
		inv_cb.on_new_session   = &OnNewSession;
		inv_cb.on_media_update  = &OnMediaUpdate;

		/* Initialize invite session module:  */
		status = pjsip_inv_usage_init(sip_endpt(), &inv_cb);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, 1);
    }

	status = pjsip_endpt_register_module(sip_endpt(), &g_monitor_module);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	status = pjsip_endpt_register_module(sip_endpt(), &g_logger_module);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	status = pjmedia_endpt_create(&caching_pool().factory, NULL, 1, &media_endpt_);
	PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

	/* Init video format manager */
    pjmedia_video_format_mgr_create(pool(), 64, 0, NULL);

    /* Init video converter manager */
    pjmedia_converter_mgr_create(pool(), NULL);

    /* Init event manager */
    pjmedia_event_mgr_create(pool(), 0, NULL);

    /* Init video codec manager */
    pjmedia_vid_codec_mgr_create(pool(), NULL);

    /* Init video subsystem */
	pjmedia_vid_dev_subsys_init(&caching_pool().factory);

	{ // Init audio and video codecs.
		status = pjmedia_codec_ffmpeg_vid_init(NULL, &caching_pool().factory);
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

		/* Must register codecs to be supported */
		status = pjmedia_codec_g711_init(media_endpt());
		PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
	}

	for (pj_uint32_t sess_idx = 0; sess_idx < sessions().size(); ++ sess_idx)
	{
		sessions()[sess_idx]->Prepare(sip_endpt(), media_endpt(), local_sip_addr_, media_start_port, g_monitor_module.id);
	}

	return status;
}

void SessionMgr::Launch()
{
	for (unsigned int i=0; i<sip_threads().size(); ++i)
	{
		pj_thread_create(pool(), "monitor", &SessionMgr::SipThread, sip_endpt(), 0, 0, &sip_threads()[i]);
    }

	for (pj_uint32_t sess_idx = 0; sess_idx < sessions().size(); ++ sess_idx)
	{
		sessions()[sess_idx]->Launch();
	}

}

pj_status_t SessionMgr::StartSession(const pj_str_t *remote_uri, pj_uint8_t sess_idx)
{
	return sessions()[sess_idx]->Invite(&local_sip_uri_, remote_uri, g_monitor_module.id); 
}

pj_status_t SessionMgr::StopSession(pj_uint8_t sess_idx)
{
	return sessions()[sess_idx]->Hangup();
}

void SessionMgr::OnStateChanged(pjsip_inv_session *inv, pjsip_event *e)
{
	Session *session = static_cast<Session *>(inv->mod_data[g_monitor_module.id]);

    PJ_UNUSED_ARG(e);

    if (!session)
	{
		return;
	}

    if (inv->state == PJSIP_INV_STATE_DISCONNECTED)
	{
		session->OnDisconnected(inv, e);
    }
	else if (inv->state == PJSIP_INV_STATE_CONFIRMED)
	{
		session->OnConfirmed(inv, e);
    }
	else if (inv->state == PJSIP_INV_STATE_EARLY ||
		inv->state == PJSIP_INV_STATE_CONNECTING)
	{
		session->OnConnecting(inv, e);
    }
}

void SessionMgr::OnNewSession(pjsip_inv_session *inv, pjsip_event *e)
{
	PJ_UNUSED_ARG(inv);
    PJ_UNUSED_ARG(e);

    PJ_TODO( HANDLE_FORKING );
}

void SessionMgr::OnMediaUpdate(pjsip_inv_session *inv, pj_status_t status)
{
    Session *session = static_cast<Session *>(inv->mod_data[g_monitor_module.id]);
	session->UpdateMedia(inv, status);
}

int SessionMgr::SipThread(void *arg)
{
	pjsip_endpoint *sip_endpt = static_cast<pjsip_endpoint *>(arg);
	pj_assert(sip_endpt != NULL);

	while ( !g_quit_flag )
	{
		pj_time_val timeout = {0, 10};
		pjsip_endpt_handle_events(sip_endpt, &timeout);
    }

	return 0;
}