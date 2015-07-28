#ifndef BUILDING_NODE_EXTENSION
	#define BUILDING_NODE_EXTENSION
#endif
#include <node.h>
#include <node_version.h>
#include <v8.h>
#include "redis-fast-driver.h"

using namespace v8;

void init(Handle<Object> exports) {
	RedisConnector::Init(exports);
}

NODE_MODULE(redis_fast_driver, init)

Persistent<Function> RedisConnector::constructor;

RedisConnector::RedisConnector(double value) : value_(value) {
	LOG("%s\n", __PRETTY_FUNCTION__);
	Local<Object> obj = NanNew<Object>();
	NanAssignPersistent(callbacks, obj);
	callback_id = 1;
}

RedisConnector::~RedisConnector() {
	LOG("%s\n", __PRETTY_FUNCTION__);
}

void RedisConnector::Init(Handle<Object> exports) {
	// Prepare constructor template
	//NanNew<FunctionTemplate>(New);
	Local<FunctionTemplate> tpl = NanNew<FunctionTemplate>(New);
	tpl->SetClassName(NanNew<String>("RedisConnector"));
	tpl->InstanceTemplate()->SetInternalFieldCount(1);
	// Prototype
	NanSetPrototypeTemplate(tpl, "connect", NanNew<FunctionTemplate>(Connect));
	NanSetPrototypeTemplate(tpl, "disconnect", NanNew<FunctionTemplate>(Disconnect));
	NanSetPrototypeTemplate(tpl, "redisCmd", NanNew<FunctionTemplate>(RedisCmd));
	NanAssignPersistent<Function>(constructor, tpl->GetFunction());
	exports->Set(NanNew("RedisConnector"), tpl->GetFunction());
}

NAN_METHOD(RedisConnector::New) {
	NanScope();

	if (args.IsConstructCall()) {
		// Invoked as constructor: `new RedisConnector(...)`
		double value = args[0]->IsUndefined() ? 0 : args[0]->NumberValue();
		RedisConnector* obj = new RedisConnector(value);
		obj->Wrap(args.This());
		args.This()->Set(NanNew<String>("callbacks"), NanNew(obj->callbacks));
		NanReturnThis();
	} else {
		// Invoked as plain function `RedisConnector(...)`, turn into construct call.
		const int argc = 1;
		Local<Value> argv[argc] = { args[0] };
		NanReturnValue(NanNew(constructor)->NewInstance(argc, argv));
		// return scope.Close(constructor->NewInstance(argc, argv));
	}
}

void RedisConnector::connectCallback(const redisAsyncContext *c, int status) {
	LOG("%s\n", __PRETTY_FUNCTION__);
	RedisConnector *self = (RedisConnector*)c->data;
	self->is_connected = true;
	if (status != REDIS_OK) {
		LOG("%s !REDIS_OK\n", __PRETTY_FUNCTION__);
		Local<Value> argv[1] = {
			NanNew<String>(c->errstr)
		};
		NanNew(self->connectCb)->Call(NanGetCurrentContext()->Global(), 1, argv);
		return;
	}
	Local<Value> argv[1] = {
		NanNull()
	};
	NanNew(self->connectCb)->Call(NanGetCurrentContext()->Global(), 1, argv);
}

void RedisConnector::disconnectCallback(const redisAsyncContext *c, int status) {
	LOG("%s\n", __PRETTY_FUNCTION__);
	RedisConnector *self = (RedisConnector*)c->data;
	self->is_connected = false;
	if (status != REDIS_OK) {
		Local<Value> argv[1] = {
			NanNew<String>(c->errstr)
		};
		NanNew(self->disconnectCb)->Call(NanGetCurrentContext()->Global(), 1, argv);
		return;
	}
	Local<Value> argv[1] = {
		NanNull()
	};
	NanNew(self->disconnectCb)->Call(NanGetCurrentContext()->Global(), 1, argv);
}

NAN_METHOD(RedisConnector::Disconnect) {
	LOG("%s\n", __PRETTY_FUNCTION__);
	NanScope();
	RedisConnector* self = ObjectWrap::Unwrap<RedisConnector>(args.This());
	if(self->c->replies.head!=NULL) {
		LOG("there is more callbacks in queue...\n");
	}
	if(self->is_connected) redisAsyncDisconnect(self->c);
	self->is_connected = false;
	self->c = NULL;
	NanReturnUndefined();
}

NAN_METHOD(RedisConnector::Connect) {
	LOG("%s\n", __PRETTY_FUNCTION__);
	NanScope();
	if(args.Length() != 4) {
		NanThrowTypeError("Wrong arguments count");
		NanReturnUndefined();
	}
	RedisConnector* self = ObjectWrap::Unwrap<RedisConnector>(args.This());
	
	String::Utf8Value v8str(args[0]);
	const char *host = *v8str;
	unsigned short port = (unsigned short)args[1]->NumberValue();
	Local<Function> connectCb = Local<Function>::Cast(args[2]);
	NanAssignPersistent(self->connectCb, connectCb);
	Local<Function> disconnectCb = Local<Function>::Cast(args[3]);
	NanAssignPersistent(self->disconnectCb, disconnectCb);
	
	if(strstr(host,"/")==host) {
		LOG("connect to unix:%s\n", host);
		self->c = redisAsyncConnectUnix(host);
	} else {
		LOG("connect to %s:%d\n", host, port);
		self->c = redisAsyncConnect(host, port);
	}
	if (self->c->err) {
		LOG("Error: %s\n", self->c->errstr);
		// handle error
		NanThrowTypeError(self->c->errstr);
		NanReturnUndefined();
	}
	uv_loop_t* loop = uv_default_loop();
	self->c->data = (void*)self;
	redisLibuvAttach(self->c,loop);
	redisAsyncSetConnectCallback(self->c,connectCallback);
	redisAsyncSetDisconnectCallback(self->c,disconnectCallback);
	
	NanReturnUndefined();
}

Local<Value> parseResponse(redisReply *reply) {
	Local<Value> resp;
	Local<Array> arr = NanNew<Array>();
	
	switch(reply->type) {
	case REDIS_REPLY_NIL:
		resp = NanNull();
		break;
	case REDIS_REPLY_INTEGER:
		resp = NanNew<Number>(reply->integer);
		break;
	case REDIS_REPLY_STATUS:
	case REDIS_REPLY_STRING:
		resp = NanNew(reply->str);
		break;
	case REDIS_REPLY_ARRAY:
		for (size_t i=0; i<reply->elements; i++) {
			arr->Set(NanNew<Number>(i), parseResponse(reply->element[i]));
		}
		resp = arr;
		break;
	default:
		printf("Redis rotocol error, unknown type %d\n", reply->type);		
		NanThrowTypeError("Protocol error, unknown type");
		return NanUndefined();
	}
	
	return resp;
}

void RedisConnector::getCallback(redisAsyncContext *c, void *r, void *privdata) {
	NanScope();
	//LOG("%s\n", __PRETTY_FUNCTION__);
	redisReply *reply = (redisReply*)r;
	uint32_t callback_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(privdata));
	if (reply == NULL) return;
	RedisConnector *self = (RedisConnector*)c->data;
	Local<Function> cb = Local<Function>::Cast(NanNew(self->callbacks)->Get(NanNew(callback_id)));
	Local<Function> setImmediate = Local<Function>::Cast(NanGetCurrentContext()->Global()->Get(NanNew("setImmediate")));
	if(!(c->c.flags & REDIS_SUBSCRIBED || c->c.flags & REDIS_MONITORING)) {
		// LOG("delete, flags %i id %i\n", c->c.flags, callback_id);
		NanNew(self->callbacks)->Delete(NanNew(callback_id)->ToString());
	} else {
		// LOG("flags %i id %i\n", c->c.flags, callback_id);
	}
	
	if (reply->type == REDIS_REPLY_ERROR) {
		//LOG("[%d] redis error: %s\n", callback_id, reply->str);
		Local<Value> argv[2] = {
			NanNew(cb),
			NanNew(reply->str)
		};
		setImmediate->Call(NanGetCurrentContext()->Global(), 2, argv);
		return;
	}
	
	Local<Value> resp = parseResponse(reply);
	if( resp->IsUndefined() ) {
		Local<Value> argv[2] = {
			NanNew(cb),
			NanNew<String>("Protocol error, can not parse answer from redis")
		};
		setImmediate->Call(NanGetCurrentContext()->Global(), 2, argv);
		return;
	}
	
	Local<Value> argv[3] = {
		NanNew(cb),
		NanNull(),
		resp
	};
	setImmediate->Call(NanGetCurrentContext()->Global(), 3, argv);
}

NAN_METHOD(RedisConnector::RedisCmd) {
	//LOG("%s\n", __PRETTY_FUNCTION__);
	NanScope();
	if(args.Length() != 2) {
		NanThrowTypeError("Wrong arguments count");
		NanReturnUndefined();
	}
	RedisConnector* self = ObjectWrap::Unwrap<RedisConnector>(args.This());
	
	Local<Array> array = Local<Array>::Cast(args[0]);
	Local<Function> cb = Local<Function>::Cast(args[1]);
	//Persistent<Function> cb = Persistent<Function>::New(Local<Function>::Cast(args[1]));
	char **argv = (char**)malloc(array->Length()*sizeof(char*));
	size_t *argvlen = (size_t*)malloc(array->Length()*sizeof(size_t*));
	uint32_t callback_id = self->callback_id++;
	NanNew(self->callbacks)->Set(NanNew<Number>(callback_id), cb);
	
	for(uint32_t i=0;i<array->Length();i++) {
		String::Utf8Value str(array->Get(i));
		argv[i] = (char*)malloc(str.length());
		memcpy(argv[i], *str, str.length());
		argvlen[i] = str.length();
		//LOG("add \"%s\" len: %d\n", argv[i], argvlen[i]);
	}
	redisAsyncCommandArgv(self->c, getCallback, (void*)(intptr_t)callback_id, array->Length(), (const char**)argv, (const size_t*)argvlen);
	for(uint32_t i=0;i<array->Length();i++) {
		free(argv[i]);
	}
	free(argv);
	free(argvlen);
	
	NanReturnUndefined();
}
