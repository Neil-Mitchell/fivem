return {
	include = function()
		includedirs 'vendor/nanomsg/include/'
	end,
	
	run = function()
		targetname 'nanomsg'
		language 'C'
		kind 'SharedLib'
		
		includedirs '../vendor/nanomsg/src/'
		
		if os.istarget('windows') then
			links { 'ws2_32', 'mswsock' }
		
			defines { 'NN_HAVE_WINDOWS', 'NN_USE_WINSOCK' }
		else
			defines { 'NN_HAVE_LINUX', 'NN_USE_EVENTFD', 'NN_USE_EPOLL', 'NN_HAVE_POLL', 'NN_HAVE_SEMAPHORE', 'NN_HAVE_MSG_CONTROL' }
		end
		
		files_project '../vendor/nanomsg/src/' {
			'nn.h',
			'inproc.h',
			'ipc.h',
			'tcp.h',
			'ws.h',
			'pair.h',
			'pubsub.h',
			'reqrep.h',
			'pipeline.h',
			'survey.h',
			'bus.h',

			'core/ep.h',
			'core/ep.c',
			'core/global.h',
			'core/global.c',
			'core/pipe.c',
			'core/poll.c',
			'core/sock.h',
			'core/sock.c',
			'core/sockbase.c',
			'core/symbol.c',

			'aio/ctx.h',
			'aio/ctx.c',
			'aio/fsm.h',
			'aio/fsm.c',
			'aio/pool.h',
			'aio/pool.c',
			'aio/timer.h',
			'aio/timer.c',
			'aio/timerset.h',
			'aio/timerset.c',
			'aio/usock.h',
			'aio/usock.c',
			'aio/worker.h',
			'aio/worker.c',

			'utils/alloc.h',
			'utils/alloc.c',
			'utils/atomic.h',
			'utils/atomic.c',
			'utils/attr.h',
			'utils/chunk.h',
			'utils/chunk.c',
			'utils/chunkref.h',
			'utils/chunkref.c',
			'utils/clock.h',
			'utils/clock.c',
			'utils/closefd.h',
			'utils/closefd.c',
			'utils/cont.h',
			'utils/efd.h',
			'utils/efd.c',
			'utils/err.h',
			'utils/err.c',
			'utils/fast.h',
			'utils/fd.h',
			'utils/hash.h',
			'utils/hash.c',
			'utils/list.h',
			'utils/list.c',
			'utils/msg.h',
			'utils/msg.c',
			'utils/condvar.h',
			'utils/condvar.c',
			'utils/mutex.h',
			'utils/mutex.c',
			'utils/once.h',
			'utils/once.c',
			'utils/queue.h',
			'utils/queue.c',
			'utils/random.h',
			'utils/random.c',
			'utils/sem.h',
			'utils/sem.c',
			'utils/sleep.h',
			'utils/sleep.c',
			'utils/strcasecmp.c',
			'utils/strcasecmp.h',
			'utils/strcasestr.c',
			'utils/strcasestr.h',
			'utils/strncasecmp.c',
			'utils/strncasecmp.h',
			'utils/thread.h',
			'utils/thread.c',
			'utils/wire.h',
			'utils/wire.c',

			'devices/device.h',
			'devices/device.c',

			'protocols/utils/dist.h',
			'protocols/utils/dist.c',
			'protocols/utils/excl.h',
			'protocols/utils/excl.c',
			'protocols/utils/fq.h',
			'protocols/utils/fq.c',
			'protocols/utils/lb.h',
			'protocols/utils/lb.c',
			'protocols/utils/priolist.h',
			'protocols/utils/priolist.c',

			'protocols/bus/bus.c',
			'protocols/bus/xbus.h',
			'protocols/bus/xbus.c',

			'protocols/pipeline/push.c',
			'protocols/pipeline/pull.c',
			'protocols/pipeline/xpull.h',
			'protocols/pipeline/xpull.c',
			'protocols/pipeline/xpush.h',
			'protocols/pipeline/xpush.c',

			'protocols/pair/pair.c',
			'protocols/pair/xpair.h',
			'protocols/pair/xpair.c',

			'protocols/pubsub/pub.c',
			'protocols/pubsub/sub.c',
			'protocols/pubsub/trie.h',
			'protocols/pubsub/trie.c',
			'protocols/pubsub/xpub.h',
			'protocols/pubsub/xpub.c',
			'protocols/pubsub/xsub.h',
			'protocols/pubsub/xsub.c',

			'protocols/reqrep/req.h',
			'protocols/reqrep/req.c',
			'protocols/reqrep/rep.h',
			'protocols/reqrep/rep.c',
			'protocols/reqrep/task.h',
			'protocols/reqrep/task.c',
			'protocols/reqrep/xrep.h',
			'protocols/reqrep/xrep.c',
			'protocols/reqrep/xreq.h',
			'protocols/reqrep/xreq.c',

			'protocols/survey/respondent.c',
			'protocols/survey/surveyor.c',
			'protocols/survey/xrespondent.h',
			'protocols/survey/xrespondent.c',
			'protocols/survey/xsurveyor.h',
			'protocols/survey/xsurveyor.c',

			'transports/utils/backoff.h',
			'transports/utils/backoff.c',
			'transports/utils/dns.h',
			'transports/utils/dns.c',
			'transports/utils/dns_getaddrinfo.h',
			'transports/utils/dns_getaddrinfo.inc',
			'transports/utils/dns_getaddrinfo_a.h',
			'transports/utils/dns_getaddrinfo_a.inc',
			'transports/utils/iface.h',
			'transports/utils/iface.c',
			'transports/utils/literal.h',
			'transports/utils/literal.c',
			'transports/utils/port.h',
			'transports/utils/port.c',
			'transports/utils/streamhdr.h',
			'transports/utils/streamhdr.c',
			'transports/utils/base64.h',
			'transports/utils/base64.c',

			'transports/inproc/binproc.h',
			'transports/inproc/binproc.c',
			'transports/inproc/cinproc.h',
			'transports/inproc/cinproc.c',
			'transports/inproc/inproc.c',
			'transports/inproc/ins.h',
			'transports/inproc/ins.c',
			'transports/inproc/msgqueue.h',
			'transports/inproc/msgqueue.c',
			'transports/inproc/sinproc.h',
			'transports/inproc/sinproc.c',

			'transports/ipc/aipc.h',
			'transports/ipc/aipc.c',
			'transports/ipc/bipc.h',
			'transports/ipc/bipc.c',
			'transports/ipc/cipc.h',
			'transports/ipc/cipc.c',
			'transports/ipc/ipc.c',
			'transports/ipc/sipc.h',
			'transports/ipc/sipc.c',

			'transports/tcp/atcp.h',
			'transports/tcp/atcp.c',
			'transports/tcp/btcp.h',
			'transports/tcp/btcp.c',
			'transports/tcp/ctcp.h',
			'transports/tcp/ctcp.c',
			'transports/tcp/stcp.h',
			'transports/tcp/stcp.c',
			'transports/tcp/tcp.c',

			'transports/ws/aws.h',
			'transports/ws/aws.c',
			'transports/ws/bws.h',
			'transports/ws/bws.c',
			'transports/ws/cws.h',
			'transports/ws/cws.c',
			'transports/ws/sws.h',
			'transports/ws/sws.c',
			'transports/ws/ws.c',
			'transports/ws/ws_handshake.h',
			'transports/ws/ws_handshake.c',
			'transports/ws/sha1.h',
			'transports/ws/sha1.c',
		}
		
		if not os.istarget('windows') then
			files_project '../vendor/nanomsg/src/' {
				'aio/poller.c',
				'aio/poller.h'
			}
		end
	end
}