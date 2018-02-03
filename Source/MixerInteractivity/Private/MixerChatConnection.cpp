#include "MixerChatConnection.h"

#include "MixerInteractivityModule.h"
#include "MixerInteractivityTypes.h"
#include "MixerInteractivityUserSettings.h"
#include "OnlineChatMixer.h"

#include "HttpModule.h"
#include "PlatformHttp.h"
#include "JsonTypes.h"
#include "JsonPrintPolicy.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "WebsocketsModule.h"
#include "IWebSocket.h"
#include "OnlineSubsystemTypes.h"

DEFINE_LOG_CATEGORY(LogMixerChat);

namespace MixerChatStringConstants
{
	namespace MessageTypes
	{
		const FString Method = TEXT("method");
		const FString Reply = TEXT("reply");
		const FString Event = TEXT("event");
	}

	namespace MethodNames
	{
		const FString Auth = TEXT("auth");
		const FString Msg = TEXT("msg");
		const FString Whisper = TEXT("whisper");
		const FString History = TEXT("history");
	}

	namespace EventTypes
	{
		const FString Welcome = TEXT("WelcomeEvent");
		const FString ChatMessage = TEXT("ChatMessage");
		const FString UserJoin = TEXT("UserJoin");
		const FString UserLeave = TEXT("UserLeave");
		const FString DeleteMessage = TEXT("DeleteMessage");
		const FString ClearMessages = TEXT("ClearMessages");
		const FString PurgeMessage = TEXT("PurgeMessage");
	}

	namespace FieldNames
	{
		const FString Type = TEXT("type");
		const FString Event = TEXT("event");
		const FString Data = TEXT("data");
		const FString Message = TEXT("message");
		const FString UserNameNoUnderscore = TEXT("username");
		const FString UserNameWithUnderscore = TEXT("user_name");
		const FString Id = TEXT("id");
		const FString Meta = TEXT("meta");
		const FString Me = TEXT("me");
		const FString Whisper = TEXT("whisper");
		const FString Method = TEXT("method");
		const FString Arguments = TEXT("arguments");
		const FString Error = TEXT("error");
		const FString Text = TEXT("text");
		const FString Endpoints = TEXT("endpoints");
		const FString AuthKey = TEXT("authkey");
		const FString UserId = TEXT("user_id");
		const FString UserLevel = TEXT("user_level");
	}
}

#define GET_JSON_FIELD_RETURN_FAILURE(JsonType, JsonNameConstant, UEType, UEName) \
UEType UEName; \
if (!JsonObj->TryGet##JsonType##Field(MixerChatStringConstants::FieldNames::##JsonNameConstant, UEName)) \
{ \
	UE_LOG(LogMixerChat, Error, TEXT("Missing required %s field in json payload"), *MixerChatStringConstants::FieldNames::##JsonNameConstant); \
	return false; \
}

#define GET_JSON_STRING_RETURN_FAILURE(JsonNameConstant, UEName)	GET_JSON_FIELD_RETURN_FAILURE(String, JsonNameConstant, FString, UEName)
#define GET_JSON_INT_RETURN_FAILURE(JsonNameConstant, UEName)		GET_JSON_FIELD_RETURN_FAILURE(Number, JsonNameConstant, int32, UEName)
#define GET_JSON_OBJECT_RETURN_FAILURE(JsonNameConstant, UEName)	GET_JSON_FIELD_RETURN_FAILURE(Object, JsonNameConstant, const TSharedPtr<FJsonObject>*, UEName)
#define GET_JSON_ARRAY_RETURN_FAILURE(JsonNameConstant, UEName)		GET_JSON_FIELD_RETURN_FAILURE(Array, JsonNameConstant, const TArray<TSharedPtr<FJsonValue>> *, UEName)

namespace
{
	template <class T, class ...ArgTypes>
	void WriteRemoteMethodParams(TJsonWriter<>& Writer, T Param1, ArgTypes... AdditionalArgs)
	{
		Writer.WriteValue(Param1);
		WriteRemoteMethodParams(Writer, AdditionalArgs...);
	}

	template <class T>
	void WriteRemoteMethodParams(TJsonWriter<>& Writer, T Param1)
	{
		Writer.WriteValue(Param1);
	}

	template <class ... ArgTypes>
	FString WriteRemoteMethodPacket(const FString& MethodName, int32 MessageId, ArgTypes... Args)
	{
		FString MethodPacketString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MethodPacketString);
		Writer->WriteObjectStart();
		Writer->WriteValue(MixerChatStringConstants::FieldNames::Type, MixerChatStringConstants::MessageTypes::Method);
		Writer->WriteValue(MixerChatStringConstants::FieldNames::Method, MethodName);
		Writer->WriteArrayStart(MixerChatStringConstants::FieldNames::Arguments);
		WriteRemoteMethodParams(Writer.Get(), Args...);
		Writer->WriteArrayEnd();
		Writer->WriteValue(MixerChatStringConstants::FieldNames::Id, MessageId);
		Writer->WriteObjectEnd();
		Writer->Close();

		return MethodPacketString;
	}
}

struct FMixerChatUser : public FMixerUser
{
public:
	FMixerChatUser(const FString& InName, int32 InId)
		: NetId(MakeShared<FUniqueNetIdMixer>(Id))
	{
		Name = InName;
		Id = InId;
		Level = 0;
	}

	const FUniqueNetIdMixer& GetUniqueNetId() const { return static_cast<FUniqueNetIdMixer&>(NetId.Get()); }

	// FChatMessage wants a old-fashioned shared ref to a net id.  Most other OSS types operate in terms
	// of native references these days.  Look for opportunities to remove this method if things change.
	const TSharedRef<const FUniqueNetId>& GetUniqueNetIdForChatMessage() const { return NetId; }

private:
	TSharedRef<FUniqueNetId> NetId;
};

struct FChatMessageMixerImpl : public FChatMessageMixer
{
public:
	FChatMessageMixerImpl(const FGuid& InMessageId, TSharedRef<const FMixerChatUser> InFromUser)
		: MessageId(InMessageId)
		, FromUser(InFromUser)
		, Timestamp(FDateTime::Now())
		, bIsWhisper(false)
		, bIsAction(false)
		, bIsModerated(false)
	{
	}

	// FChatMessage methods
	virtual const TSharedRef<const FUniqueNetId>& GetUserId() const override	{ return FromUser->GetUniqueNetIdForChatMessage(); }
	virtual const FString& GetNickname() const override							{ return FromUser->Name; }
	virtual const FString& GetBody() const override								{ return Body; }
	virtual const FDateTime& GetTimestamp() const override						{ return Timestamp; }

	// FChatMessageMixer methods
	virtual bool IsWhisper() override		{ return bIsWhisper; }
	virtual bool IsAction() override		{ return bIsAction; }
	virtual bool IsModerated() override		{ return bIsModerated; }

	const FMixerChatUser& GetSender() { return FromUser.Get(); }
	const FGuid& GetMessageId() { return MessageId; }

	void FlagAsDeleted()
	{
		Body.Empty();
		bIsModerated = true;
	}

	void AppendBodyFragment(const FString& InBodyFragment)
	{
		Body += InBodyFragment;
	}

	void FlagAsWhisper()
	{
		bIsWhisper = true;
	}

	void FlagAsAction()
	{
		if (!bIsAction)
		{
			bIsAction = true;
			Body = FromUser->Name + TEXT(" ") + Body;
		}
	}

private:
	FGuid MessageId;
	TSharedRef<const FMixerChatUser> FromUser;
	FString Body;
	FDateTime Timestamp;
	bool bIsWhisper;
	bool bIsAction;
	bool bIsModerated;

public:
	// Intrusive list to avoid double allocation for chat history
	// Would be nice to use TIntrusiveList, but that doesn't support
	// TSharedPtr as the link type, and IOnlineChat enforces that the
	// lifetime is managed by TSharedPtr
	TSharedPtr<FChatMessageMixerImpl> NextLink;
	TSharedPtr<FChatMessageMixerImpl> PrevLink;
};

FMixerChatConnection::~FMixerChatConnection()
{
	CloseWebSocket();
}

bool FMixerChatConnection::Init()
{
#if WITH_WEBSOCKETS
	TSharedRef<IHttpRequest> ChannelRequest = FHttpModule::Get().CreateRequest();
	ChannelRequest->SetVerb(TEXT("GET"));
	ChannelRequest->SetURL(FString::Printf(TEXT("https://mixer.com/api/v1/channels/%s"), *RoomId));

	ChannelRequest->OnProcessRequestComplete().BindSP(this, &FMixerChatConnection::OnGetChannelInfoForRoomIdComplete);
	return ChannelRequest->ProcessRequest();
#else
	UE_LOG(LogMixerChat, Warning, TEXT("Mixer chat requires websockets which are not available on this platform."));
	return false;
#endif
}

void FMixerChatConnection::JoinDiscoveredChatChannel()
{
	TSharedRef<IHttpRequest> ChatRequest = FHttpModule::Get().CreateRequest();
	ChatRequest->SetVerb(TEXT("GET"));
	ChatRequest->SetURL(FString::Printf(TEXT("https://mixer.com/api/v1/chats/%d?fields=id"), ChannelId));

	// Setting Authorization header to an empty string will just fail rather than perform anonymous auth.
	const UMixerInteractivityUserSettings* UserSettings = GetDefault<UMixerInteractivityUserSettings>();
	FString AuthZHeaderValue = UserSettings->GetAuthZHeaderValue();
	if (AuthZHeaderValue.Len() > 0)
	{
		ChatRequest->SetHeader(TEXT("Authorization"), AuthZHeaderValue);
	}
	else
	{
		UE_LOG(LogMixerChat, Warning, TEXT("No auth token found.  Chat connection will be anonymous and will not allow sending messages.  Sign in to Mixer to enable."));
	}

	ChatRequest->OnProcessRequestComplete().BindSP(this, &FMixerChatConnection::OnDiscoverChatServersComplete);
	if (!ChatRequest->ProcessRequest())
	{
		ChatInterface->ConnectAttemptFinished(*User, RoomId, false, TEXT("Failed to send request for chat web socket connection info."));

		// Note: we have probably self-destructed at this point
	}
}

void FMixerChatConnection::OnGetChannelInfoForRoomIdComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
{
	if (bSucceeded && HttpResponse.IsValid())
	{
		if (EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
		{
			FString ResponseStr = HttpResponse->GetContentAsString();
			TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ResponseStr);
			TSharedPtr<FJsonObject> JsonObject;
			if (FJsonSerializer::Deserialize(JsonReader, JsonObject) &&
				JsonObject.IsValid())
			{
				JsonObject->TryGetNumberField(MixerChatStringConstants::FieldNames::Id, ChannelId);
			}
		}
	}

	if (ChannelId != 0)
	{
		JoinDiscoveredChatChannel();
	}
	else
	{
		ChatInterface->ConnectAttemptFinished(*User, RoomId, false, TEXT("Could not find Mixer chat channel for room id."));

		// Note: we have probably self-destructed at this point
	}
}

void FMixerChatConnection::OnDiscoverChatServersComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
{
	if (bSucceeded && HttpResponse.IsValid())
	{
		if (EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
		{
			FString ResponseStr = HttpResponse->GetContentAsString();
			TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ResponseStr);
			TSharedPtr<FJsonObject> JsonObject;
			if (FJsonSerializer::Deserialize(JsonReader, JsonObject) &&
				JsonObject.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>> *JsonEndpoints;
				if (JsonObject->TryGetArrayField(MixerChatStringConstants::FieldNames::Endpoints, JsonEndpoints))
				{
					for (const TSharedPtr<FJsonValue>& Endpoint : *JsonEndpoints)
					{
						Endpoints.Add(Endpoint->AsString());
					}

					JsonObject->TryGetStringField(MixerChatStringConstants::FieldNames::AuthKey, AuthKey);
					OpenWebSocket();
				}
			}
		}
	}

	// Should have a web socket going by now.
	if (!WebSocket.IsValid())
	{
		ChatInterface->ConnectAttemptFinished(*User, RoomId, false, TEXT("Failed to create web socket"));

		// Note: we have probably self-destructed at this point
	}
}

void FMixerChatConnection::OnChatSocketConnected()
{
	TSharedPtr<const FMixerLocalUser> CurrentUser = IMixerInteractivityModule::Get().GetCurrentUser();
	SendAuth(ChannelId, CurrentUser.Get(), AuthKey);
}

void FMixerChatConnection::OnChatConnectionError(const FString& ErrorMessage)
{
	UE_LOG(LogMixerChat, Warning, TEXT("Failed to connect chat web socket for room %s with error '%s'"), *RoomId, *ErrorMessage);
	ChatInterface->ConnectAttemptFinished(*User, RoomId, false, ErrorMessage);

	// Note: we have probably self-destructed at this point
}

void FMixerChatConnection::OnChatSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	// This should be a remote close since we unhook event handlers before closing on our end.
	// Do a full close and re-open of the websocket so as to (potentially) hit a different endpoint, per Mixer guidance.

	UE_LOG(LogMixerChat, Warning, TEXT("Chat websocket closed with reason '%s'."), *Reason);

	bool bWasReady = bIsReady;

	CloseWebSocket();

	if (bRejoinOnDisconnect)
	{
		UE_LOG(LogMixerChat, Warning, TEXT("Attempting automatic reconnect to %s."), *RoomId);
		OpenWebSocket();
	}
	else if (bWasReady)
	{
		ChatInterface->ExitRoomWithReason(*User, RoomId, bWasClean, Reason);

		// Note: we have probably self-destructed at this point
	}
	else 
	{
		ChatInterface->ConnectAttemptFinished(*User, RoomId, false, Reason);

		// Note: we have probably self-destructed at this point
	}
}

void FMixerChatConnection::OnChatPacket(const FString& PacketJsonString)
{
	bool bHandled = false;
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(PacketJsonString);
	TSharedPtr<FJsonObject> JsonObj;
	if (FJsonSerializer::Deserialize(JsonReader, JsonObj) && JsonObj.IsValid())
	{
		bHandled = OnChatPacketInternal(JsonObj.Get());
	}

	if (!bHandled)
	{
		UE_LOG(LogMixerChat, Error, TEXT("Failed to handle chat packet from server: %s"), *PacketJsonString);
	}
}

bool FMixerChatConnection::OnChatPacketInternal(FJsonObject* JsonObj)
{
	bool bHandled = false;
	GET_JSON_STRING_RETURN_FAILURE(Type, MessageType);
	if (MessageType == MixerChatStringConstants::MessageTypes::Reply)
	{
		GET_JSON_INT_RETURN_FAILURE(Id, ReplyingToMessageId);

		FServerMessageHandler Handler;
		if (ReplyHandlers.RemoveAndCopyValue(ReplyingToMessageId, Handler))
		{
			if (Handler != nullptr)
			{
				(this->*Handler)(JsonObj);
			}
			bHandled = true;
		}
		else
		{
			UE_LOG(LogMixerChat, Error, TEXT("Received unexpected reply for unknown message id %d"), ReplyingToMessageId);
		}
	}
	else if (MessageType == MixerChatStringConstants::MessageTypes::Event)
	{
		GET_JSON_STRING_RETURN_FAILURE(Event, EventType);
		GET_JSON_OBJECT_RETURN_FAILURE(Data, Data);

		FServerMessageHandler Handler = GetEventHandler(EventType);
		if (Handler != nullptr)
		{
			(this->*Handler)(Data->Get());
			bHandled = true;
		}
		else
		{
			UE_LOG(LogMixerChat, Warning, TEXT("Received event type %s which is not handled in the current implementation."), *EventType);
		}
	}

	return bHandled;
}

bool FMixerChatConnection::HandleWelcomeEvent(class FJsonObject* JsonObj)
{
	// Welcomed by the server.  We are now fully connected.
	// But we have not necessarily completed auth.  That means we should use the
	// reply to the auth method call (which occurs even for anonymous connections)
	// to trigger the join event, otherwise callers might initially see operations
	// that require auth fail.

	UE_LOG(LogMixerChat, Log, TEXT("Welcomed by chat server for %s"), *RoomId);

	// Currently that means there's nothing to do here.
	return true;
}


bool FMixerChatConnection::HandleChatMessageEvent(FJsonObject* JsonObj)
{
	TSharedPtr<FChatMessageMixerImpl> ChatMessage;
	bool bHandled = HandleChatMessageEventInternal(JsonObj, ChatMessage);

	if (bHandled)
	{
		check(ChatMessage.IsValid());
		if (ChatMessage->IsWhisper())
		{
			UE_LOG(LogMixerChat, Verbose, TEXT("Private message from %s: %s"), *ChatMessage->GetNickname(), *ChatMessage->GetBody());
			ChatInterface->TriggerOnChatPrivateMessageReceivedDelegates(*User, ChatMessage.ToSharedRef());
		}
		else
		{
			UE_LOG(LogMixerChat, Verbose, TEXT("Chat message from %s in room %s: %s"), *ChatMessage->GetNickname(), *RoomId, *ChatMessage->GetBody());
			AddMessageToChatHistory(ChatMessage.ToSharedRef());
			ChatInterface->TriggerOnChatRoomMessageReceivedDelegates(*User, RoomId, ChatMessage.ToSharedRef());
		}
	}

	return bHandled;
}

bool FMixerChatConnection::HandleChatMessageEventInternal(FJsonObject* JsonObj, TSharedPtr<FChatMessageMixerImpl>& OutChatMessage)
{
	GET_JSON_STRING_RETURN_FAILURE(UserNameWithUnderscore, FromUserName);
	GET_JSON_INT_RETURN_FAILURE(UserId, FromUserIdRaw);
	GET_JSON_OBJECT_RETURN_FAILURE(Message, MessageJson);
	GET_JSON_STRING_RETURN_FAILURE(Id, IdString);

	FGuid MessageId;
	if (!FGuid::Parse(IdString, MessageId))
	{
		UE_LOG(LogMixerChat, Error, TEXT("id field %s for chat event was not in the expected format (guid)"), *IdString);
		return false;
	}

	FUniqueNetIdMixer FromNetIdLocal = FUniqueNetIdMixer(FromUserIdRaw);
	TSharedPtr<FMixerChatUser>* FromUserObject = CachedUsers.Find(FromNetIdLocal);
	bool bSendJoinEvent = false;
	if (FromUserObject == nullptr)
	{
		TSharedRef<FMixerChatUser> NewUser = MakeShared<FMixerChatUser>(FromUserName, FromUserIdRaw);
		FromUserObject = &CachedUsers.Add(FromNetIdLocal, NewUser);

		// We haven't seen this user before - send a just-in-time join event,
		// but wait until after we have resolved the user level
		bSendJoinEvent = true;
	}
	check(FromUserObject);
	if (!JsonObj->TryGetNumberField(MixerChatStringConstants::FieldNames::UserLevel, (*FromUserObject)->Level))
	{
		// This one's less serious.
		UE_LOG(LogMixerChat, Warning, TEXT("Missing user_level field for chat event"));
	}

	if (bSendJoinEvent)
	{
		UE_LOG(LogMixerChat, Log, TEXT("%s is joining %s's chat channel"), *(*FromUserObject)->Name, *RoomId);

		ChatInterface->TriggerOnChatRoomMemberJoinDelegates(*User, RoomId, (*FromUserObject)->GetUniqueNetId());
	}

	OutChatMessage = MakeShared<FChatMessageMixerImpl>(MessageId, FromUserObject->ToSharedRef());
	return HandleChatMessageEventMessageObject(MessageJson->Get(), OutChatMessage.Get());
}

bool FMixerChatConnection::HandleChatMessageEventMessageObject(FJsonObject* JsonObj, FChatMessageMixerImpl* ChatMessage)
{
	GET_JSON_ARRAY_RETURN_FAILURE(Message, MessageFragmentArray);

	for (const TSharedPtr<FJsonValue>& Fragment : *MessageFragmentArray)
	{
		const TSharedPtr<FJsonObject>* FragmentObj;
		if (Fragment->TryGetObject(FragmentObj))
		{
			HandleChatMessageEventMessageArrayEntry(FragmentObj->Get(), ChatMessage);
		}
	}

	// These are not required.
	const TSharedPtr<FJsonObject>* Metadata;
	bool bIsWhisper = false;
	bool bIsAction = false;
	if (JsonObj->TryGetObjectField(MixerChatStringConstants::FieldNames::Meta, Metadata))
	{
		JsonObj->TryGetBoolField(MixerChatStringConstants::FieldNames::Whisper, bIsWhisper);
		JsonObj->TryGetBoolField(MixerChatStringConstants::FieldNames::Me, bIsAction);
	}

	if (bIsWhisper)
	{
		ChatMessage->FlagAsWhisper();
	}

	if (bIsAction)
	{
		ChatMessage->FlagAsAction();
	}

	return true;
}

bool FMixerChatConnection::HandleChatMessageEventMessageArrayEntry(FJsonObject* JsonObj, FChatMessageMixerImpl* ChatMessage)
{
	GET_JSON_STRING_RETURN_FAILURE(Type, FragmentType);
	GET_JSON_STRING_RETURN_FAILURE(Text, FragmentText);

	// For now just always append the fragment text no matter the type.
	// In the future we could perhaps add markup?
	ChatMessage->AppendBodyFragment(FragmentText);
	return true;
}

bool FMixerChatConnection::HandleUserJoinEvent(FJsonObject* JsonObj)
{
	GET_JSON_INT_RETURN_FAILURE(Id, JoiningUserIdRaw);

	FUniqueNetIdMixer JoiningNetId = FUniqueNetIdMixer(JoiningUserIdRaw);
	TSharedPtr<FMixerChatUser>* CachedUser = CachedUsers.Find(JoiningNetId);

	// If the user was already in the cache then we triggered a join event at the
	// point of addition (presumably a chat message reached us before join?).  Don't
	// send another.
	if (CachedUser == nullptr)
	{
		GET_JSON_STRING_RETURN_FAILURE(UserNameNoUnderscore, JoiningUserName);
		CachedUser = &CachedUsers.Add(JoiningNetId, MakeShared<FMixerChatUser>(JoiningUserName, JoiningUserIdRaw));

		UE_LOG(LogMixerChat, Log, TEXT("%s is joining %s's chat channel"), *(*CachedUser)->Name, *RoomId);
		ChatInterface->TriggerOnChatRoomMemberJoinDelegates(*User, RoomId, (*CachedUser)->GetUniqueNetId());
	}

	return true;
}

bool FMixerChatConnection::HandleUserLeaveEvent(FJsonObject* JsonObj)
{
	GET_JSON_INT_RETURN_FAILURE(Id, LeavingUserIdRaw);

	FUniqueNetIdMixer LeavingNetId = FUniqueNetIdMixer(LeavingUserIdRaw);
	TSharedPtr<FMixerChatUser> LeavingUser;

	// If we never cached the user then we never triggered a join event, in which
	// case we shouldn't trigger leave either.
	if (CachedUsers.RemoveAndCopyValue(LeavingNetId, LeavingUser))
	{
		UE_LOG(LogMixerChat, Log, TEXT("%s is exiting %s's chat channel"), *LeavingUser->Name, *RoomId);

		ChatInterface->TriggerOnChatRoomMemberExitDelegates(*User, RoomId, LeavingUser->GetUniqueNetId());
	}

	return true;
}

bool FMixerChatConnection::HandleDeleteMessageEvent(FJsonObject* JsonObj)
{
	GET_JSON_STRING_RETURN_FAILURE(Id, IdString);

	FGuid MessageId;
	if (!FGuid::Parse(IdString, MessageId))
	{
		UE_LOG(LogMixerChat, Error, TEXT("id field %s for delete message event was not in the expected format (guid)"), *IdString);
		return false;
	}

	DeleteFromChatHistoryIf([&MessageId](TSharedPtr<FChatMessageMixerImpl> ChatMessage)
	{
		return ChatMessage->GetMessageId() == MessageId;
	});

	return true;
}

bool FMixerChatConnection::HandleClearMessagesEvent(FJsonObject* JsonObj)
{
	DeleteFromChatHistoryIf([](TSharedPtr<FChatMessageMixerImpl>)
	{
		return true;
	});

	check(ChatHistoryNum == 0);
	check(!ChatHistoryNewest.IsValid());
	check(!ChatHistoryOldest.IsValid());

	return true;
}

bool FMixerChatConnection::HandlePurgeMessageEvent(FJsonObject* JsonObj)
{
	GET_JSON_INT_RETURN_FAILURE(UserId, UserId);

	DeleteFromChatHistoryIf([UserId](TSharedPtr<FChatMessageMixerImpl> ChatMessage)
	{
		return ChatMessage->GetSender().Id == UserId;
	});

	return true;
}

bool FMixerChatConnection::SendChatMessage(const FString& MessageBody)
{
	if (!bIsReady)
	{
		UE_LOG(LogMixerChat, Warning, TEXT("Attempt to send chat to room %s before connection has been established.  Wait for OnChatRoomJoin event."), *RoomId);
		return false;
	}

	if (IsAnonymous())
	{
		UE_LOG(LogMixerChat, Warning, TEXT("Attempt to send chat to room %s when connected anonymously."), *RoomId);
		return false;
	}

	check(WebSocket.IsValid());
	check(WebSocket->IsConnected());

	FString MethodPacket = WriteRemoteMethodPacket(MixerChatStringConstants::MethodNames::Msg, MessageId, MessageBody);
	SendMethodPacket(MethodPacket, nullptr);

	return true;
}

bool FMixerChatConnection::SendWhisper(const FString& ToUser, const FString& MessageBody)
{
	if (!bIsReady)
	{
		UE_LOG(LogMixerChat, Warning, TEXT("Attempt to send chat to room %s before connection has been established.  Wait for OnChatRoomJoin event."), *RoomId);
		return false;
	}

	if (IsAnonymous())
	{
		UE_LOG(LogMixerChat, Warning, TEXT("Attempt to send chat to room %s when connected anonymously."), *RoomId);
		return false;
	}

	FString MethodPacket = WriteRemoteMethodPacket(MixerChatStringConstants::MethodNames::Whisper, MessageId, ToUser, MessageBody);
	SendMethodPacket(MethodPacket, nullptr);

	return true;
}

void FMixerChatConnection::SendAuth(int32 ChannelId, const FMixerLocalUser* User, const FString& AuthKey)
{
	FString MethodPacket;
	if (User != nullptr && !AuthKey.IsEmpty())
	{
		UE_LOG(LogMixerChat, Log, TEXT("Authenticating to chat room %s as user '%s'"), *RoomId, *User->Name);
		MethodPacket = WriteRemoteMethodPacket(MixerChatStringConstants::MethodNames::Auth, MessageId, ChannelId, User->Id, AuthKey);
	}
	else
	{
		UE_LOG(LogMixerChat, Log, TEXT("Authenticating to chat room %s anonymously"), *RoomId);
		MethodPacket = WriteRemoteMethodPacket(MixerChatStringConstants::MethodNames::Auth, MessageId, ChannelId);
	}
	SendMethodPacket(MethodPacket, &FMixerChatConnection::HandleAuthReply);
}

void FMixerChatConnection::SendHistory(int32 MessageCount)
{
	FString MethodPacket = WriteRemoteMethodPacket(MixerChatStringConstants::MethodNames::History, MessageId, MessageCount);
	SendMethodPacket(MethodPacket, &FMixerChatConnection::HandleHistoryReply);
}

void FMixerChatConnection::SendMethodPacket(const FString& Payload, FServerMessageHandler Handler)
{
	check(!ReplyHandlers.Contains(MessageId));
	ReplyHandlers.Add(MessageId, Handler);
	++MessageId;

	WebSocket->Send(Payload);
}

void FMixerChatConnection::GetMessageHistory(int32 NumMessages, TArray< TSharedRef<FChatMessage> >& OutMessages)
{
	TSharedPtr<FChatMessageMixerImpl> ChatMessage = ChatHistoryNewest;
	while (ChatMessage.IsValid() &&
		(NumMessages == -1 || OutMessages.Num() < NumMessages))
	{
		OutMessages.Add(ChatMessage.ToSharedRef());
		ChatMessage = ChatMessage->NextLink;
	}
}


void FMixerChatConnection::OpenWebSocket()
{
	// Shouldn't ever get this far if we don't have a websocket implementation.
	check(WITH_WEBSOCKETS != 0);
	const FString& SelectedEndpoint = Endpoints[FMath::RandRange(0, Endpoints.Num() - 1)];
	UE_LOG(LogMixerChat, Verbose, TEXT("Opening web socket to %s for chat room %s"), *SelectedEndpoint, *RoomId);

	// Regardless, CreateWebSocket won't compile on all platforms.
#if WITH_WEBSOCKETS
	// Explicitly list protocols for the benefit of Xbox
	TArray<FString> Protocols;
	Protocols.Add(TEXT("wss"));
	Protocols.Add(TEXT("ws"));
	WebSocket = FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets").CreateWebSocket(SelectedEndpoint, Protocols);
#endif

	if (WebSocket.IsValid())
	{
		WebSocket->OnConnected().AddRaw(this, &FMixerChatConnection::OnChatSocketConnected);
		WebSocket->OnConnectionError().AddRaw(this, &FMixerChatConnection::OnChatConnectionError);
		WebSocket->OnMessage().AddRaw(this, &FMixerChatConnection::OnChatPacket);
		WebSocket->OnClosed().AddRaw(this, &FMixerChatConnection::OnChatSocketClosed);

		WebSocket->Connect();
	}
}

void FMixerChatConnection::CloseWebSocket()
{
	if (WebSocket.IsValid())
	{
		bIsReady = false;

		WebSocket->OnConnected().RemoveAll(this);
		WebSocket->OnConnectionError().RemoveAll(this);
		WebSocket->OnMessage().RemoveAll(this);
		WebSocket->OnClosed().RemoveAll(this);

		if (WebSocket->IsConnected())
		{
			WebSocket->Close();
		}

		WebSocket.Reset();
	}
}

void FMixerChatConnection::AddMessageToChatHistory(TSharedRef<FChatMessageMixerImpl> ChatMessage)
{
	if (ChatHistoryMax > 0 && !ChatMessage->IsWhisper())
	{
		ChatMessage->NextLink = ChatHistoryNewest;
		if (ChatHistoryNewest.IsValid())
		{
			ChatHistoryNewest->PrevLink = ChatMessage;
		}
		ChatHistoryNewest = ChatMessage;
		++ChatHistoryNum;
		if (!ChatHistoryOldest.IsValid())
		{
			ChatHistoryOldest = ChatMessage;
		}
		else if (ChatHistoryNum > ChatHistoryMax)
		{
			ChatHistoryOldest = ChatHistoryOldest->PrevLink;
			if (ChatHistoryOldest.IsValid())
			{
				check(ChatHistoryOldest->NextLink.IsValid());
				ChatHistoryOldest->NextLink->PrevLink.Reset();
				ChatHistoryOldest->NextLink.Reset();
			}
			--ChatHistoryNum;
		}
	}
}

void FMixerChatConnection::DeleteFromChatHistoryIf(TFunctionRef<bool(TSharedPtr<FChatMessageMixerImpl>)> Predicate)
{
	// @TODO - pass moderator here when available
	TSharedPtr<FChatMessageMixerImpl> ChatMessage = ChatHistoryNewest;
	while (ChatMessage.IsValid())
	{
		TSharedPtr<FChatMessageMixerImpl> NextMessage = ChatMessage->NextLink;
		if (Predicate(ChatMessage))
		{
			ChatMessage->FlagAsDeleted();
			if (ChatMessage == ChatHistoryNewest)
			{
				ChatHistoryNewest = ChatMessage->NextLink;
			}
			if (ChatMessage == ChatHistoryOldest)
			{
				ChatHistoryOldest = ChatMessage->PrevLink;
			}
			if (ChatMessage->NextLink.IsValid())
			{
				ChatMessage->NextLink->PrevLink = ChatMessage->PrevLink;
			}
			if (ChatMessage->PrevLink.IsValid())
			{
				ChatMessage->PrevLink->NextLink = ChatMessage->NextLink;
			}
			ChatMessage->NextLink.Reset();
			ChatMessage->PrevLink.Reset();
			--ChatHistoryNum;
		}
		ChatMessage = NextMessage;
	}
}


bool FMixerChatConnection::HandleAuthReply(FJsonObject* JsonObj)
{
	const TSharedPtr<FJsonObject>* Error;
	if (JsonObj->TryGetObjectField(MixerChatStringConstants::FieldNames::Error, Error))
	{
		FString ErrorMessage;
		(*Error)->TryGetStringField(MixerChatStringConstants::FieldNames::Message, ErrorMessage);
		ChatInterface->ConnectAttemptFinished(*User, RoomId, false, ErrorMessage);

		// Note: we have probably self-destructed at this point
		return false;
	}
	else
	{
		bIsReady = true;
		if (ChatHistoryMax > 0)
		{
			SendHistory(FMath::Min(ChatHistoryMax, 100));
		}
		// Maybe we have some interest in roles?

		ChatInterface->ConnectAttemptFinished(*User, RoomId, true, FString());

		return true;
	}
}

bool FMixerChatConnection::HandleHistoryReply(FJsonObject* JsonObj)
{
	GET_JSON_ARRAY_RETURN_FAILURE(Data, Data);

	// Stash the current history and then clear member pointers.
	// We'll splice what we have back on the front of the history
	// reported by the server.
	TSharedPtr<FChatMessageMixerImpl> LocalHistoryNewest = ChatHistoryNewest;
	TSharedPtr<FChatMessageMixerImpl> LocalHistoryOldest = ChatHistoryOldest;
	int32 LocalHistoryNum = ChatHistoryNum;
	int32 LocalHistoryMax = ChatHistoryMax;
	ChatHistoryNewest.Reset();
	ChatHistoryOldest.Reset();
	ChatHistoryNum = 0;
	ChatHistoryMax = LocalHistoryMax - LocalHistoryNum;

	// Oldest entry is at index 0 as reported by Mixer
	// Whereas we keep the newest entry at the head of the list,
	// which is where HandleChatMessagePacket pushes.
	for (const TSharedPtr<FJsonValue> HistoryEntry : *Data)
	{
		TSharedPtr<FChatMessageMixerImpl> ChatMessage;
		if (HandleChatMessageEventInternal(HistoryEntry->AsObject().Get(), ChatMessage))
		{
			check(!ChatMessage->IsWhisper());
			AddMessageToChatHistory(ChatMessage.ToSharedRef());
		}
	}

	// Relink the history we'd already accumulated.
	// Possibly our history request crossed paths with some
	// new messages and we could have some dupes?
	if (!ChatHistoryNewest.IsValid())
	{
		ChatHistoryNewest = LocalHistoryNewest;
		ChatHistoryOldest = LocalHistoryOldest;
	}
	else if (LocalHistoryOldest.IsValid())
	{
		int32 DupeCount = 0;
		FGuid IdToCheckForDupes = LocalHistoryOldest->GetMessageId();
		TSharedPtr<FChatMessageMixerImpl> ServerHistoryMessage = ChatHistoryNewest;
		while (ServerHistoryMessage.IsValid())
		{
			++DupeCount;
			if (ServerHistoryMessage->GetMessageId() == IdToCheckForDupes)
			{
				break;
			}
			ServerHistoryMessage = ServerHistoryMessage->NextLink;
		}

		if (ServerHistoryMessage.IsValid())
		{
			LocalHistoryOldest->NextLink = ServerHistoryMessage->NextLink;
			if (ServerHistoryMessage->NextLink.IsValid())
			{
				ServerHistoryMessage->NextLink->PrevLink = LocalHistoryOldest;
			}
			ChatHistoryNum -= DupeCount;
		}
		else
		{
			LocalHistoryOldest->NextLink = ChatHistoryNewest;
			ChatHistoryNewest->PrevLink = LocalHistoryOldest;
		}

		ChatHistoryNewest = LocalHistoryNewest;
		ChatHistoryNum += LocalHistoryNum;
		ChatHistoryMax = LocalHistoryMax;
	}

	return true;
}

FMixerChatConnection::FServerMessageHandler FMixerChatConnection::GetEventHandler(const FString& EventType)
{
	static TMap<const FString, FServerMessageHandler> EventHandlers;
	if (EventHandlers.Num() == 0)
	{
		EventHandlers.Add(MixerChatStringConstants::EventTypes::Welcome, &FMixerChatConnection::HandleWelcomeEvent);
		EventHandlers.Add(MixerChatStringConstants::EventTypes::ChatMessage, &FMixerChatConnection::HandleChatMessageEvent);
		EventHandlers.Add(MixerChatStringConstants::EventTypes::UserJoin, &FMixerChatConnection::HandleUserJoinEvent);
		EventHandlers.Add(MixerChatStringConstants::EventTypes::UserLeave, &FMixerChatConnection::HandleUserLeaveEvent);
		EventHandlers.Add(MixerChatStringConstants::EventTypes::DeleteMessage, &FMixerChatConnection::HandleDeleteMessageEvent);
		EventHandlers.Add(MixerChatStringConstants::EventTypes::ClearMessages, &FMixerChatConnection::HandleClearMessagesEvent);
		EventHandlers.Add(MixerChatStringConstants::EventTypes::PurgeMessage, &FMixerChatConnection::HandlePurgeMessageEvent);
	}

	FServerMessageHandler* Handler = EventHandlers.Find(EventType);
	return Handler != nullptr ? *Handler : nullptr;
}


