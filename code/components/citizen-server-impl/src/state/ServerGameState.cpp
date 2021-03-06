#include <StdInc.h>
#include <GameServer.h>

#include <state/ServerGameState.h>

#include <optional>

#include <NetBuffer.h>

#include <lz4.h>

#include <state/Pool.h>

CPool<fx::ScriptGuid>* g_scriptHandlePool;

std::shared_ptr<ConVar<bool>> g_oneSyncVar;

namespace fx
{
struct GameStateClientData
{
	net::Buffer ackBuffer;
	std::set<int> objectIds;
};

inline std::shared_ptr<GameStateClientData> GetClientData(ServerGameState* state, const std::shared_ptr<fx::Client>& client)
{
	auto data = client->GetData("gameStateClientData");

	if (data.has_value())
	{
		return std::any_cast<std::shared_ptr<GameStateClientData>>(data);
	}

	auto val = std::make_shared<GameStateClientData>();
	client->SetData("gameStateClientData", val);

	std::weak_ptr<fx::Client> weakClient(client);

	client->OnDrop.Connect([weakClient, state]()
	{
		state->HandleClientDrop(weakClient.lock());
	});


	return val;
}

inline uint32_t MakeEntityHandle(uint8_t playerId, uint16_t objectId)
{
	return ((playerId + 1) << 16) | objectId;
}

static uint32_t MakeScriptHandle(const std::shared_ptr<sync::SyncEntityState>& ptr)
{
	if (!ptr->guid)
	{
		// find an existing handle (transformed TempEntity?)
		for (int i = 0; i < g_scriptHandlePool->m_Size; i++)
		{
			auto hdl = g_scriptHandlePool->GetAt(i);

			if (hdl && hdl->type == ScriptGuid::Type::Entity && hdl->entity.handle == ptr->handle)
			{
				ptr->guid = hdl;
			}
		}

		if (!ptr->guid)
		{
			auto guid = g_scriptHandlePool->New();
			guid->type = ScriptGuid::Type::Entity;
			guid->entity.handle = ptr->handle;

			ptr->guid = guid;
		}
	}

	return g_scriptHandlePool->GetIndex(ptr->guid) + 0x20000;
}

std::shared_ptr<sync::SyncEntityState> ServerGameState::GetEntity(uint8_t playerId, uint16_t objectId)
{
	auto handle = MakeEntityHandle(playerId, objectId);

	auto it = m_entities.find(handle);

	if (it != m_entities.end())
	{
		return it->second;
	}

	return {};
}

std::shared_ptr<sync::SyncEntityState> ServerGameState::GetEntity(uint32_t guid)
{
	// subtract the minimum index GUID
	guid -= 0x20000;

	// get the pool entry
	auto guidData = g_scriptHandlePool->AtHandle(guid);

	if (guidData)
	{
		if (guidData->type == ScriptGuid::Type::Entity)
		{
			auto it = m_entities.find(guidData->entity.handle);

			if (it != m_entities.end())
			{
				return it->second;
			}
		}
	}

	return {};
}

namespace sync
{
	SyncEntityState::~SyncEntityState()
	{
		if (guid)
		{
			g_scriptHandlePool->Delete(guid);

			guid = nullptr;
		}
	}
}

void ServerGameState::Tick(fx::ServerInstanceBase* instance)
{
	instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&](const std::shared_ptr<fx::Client>& client)
	{
		if (!client->GetData("playerId").has_value())
		{
			return;
		}

		auto clientData = GetClientData(this, client);

		{
			auto& ackPacket = clientData->ackBuffer;

			// any ACKs to send?
			if (ackPacket.GetCurOffset() > 4)
			{
				client->SendPacket(0, ackPacket, ENET_PACKET_FLAG_RELIABLE);
				ackPacket.Reset();
			}
		}

		rl::MessageBuffer cloneBuffer(16384);

		auto flushBuffer = [&]()
		{
			if (cloneBuffer.GetDataLength() > 0)
			{
				// end
				cloneBuffer.Write(3, 7);

				// compress and send
				std::vector<char> outData(LZ4_compressBound(cloneBuffer.GetDataLength()) + 4);
				int len = LZ4_compress_default(reinterpret_cast<const char*>(cloneBuffer.GetBuffer().data()), outData.data() + 4, cloneBuffer.GetDataLength(), outData.size() - 4);

				*(uint32_t*)(outData.data()) = HashRageString("msgPackedClones");

				net::Buffer netBuffer(reinterpret_cast<uint8_t*>(outData.data()), len + 4);			

				client->SendPacket(1, netBuffer);

				cloneBuffer.SetCurrentBit(0);
			}
		};

		auto maybeFlushBuffer = [&]()
		{
			if (LZ4_compressBound(cloneBuffer.GetDataLength()) > 1100)
			{
				flushBuffer();
			}
		};

		int numCreates = 0, numSyncs = 0, numSkips = 0;

		for (auto& entityPair : m_entities)
		{
			auto entity = entityPair.second;

			if (!entity || !entity->syncTree)
			{
				continue;
			}

			if (entity->client.expired())// || client->GetNetId() == entity->client.lock()->GetNetId())
			{
				continue; 
			}

			bool hasCreated = entity->ackedCreation.test(client->GetSlotId()) || client->GetNetId() == entity->client.lock()->GetNetId();

			auto sendUnparsedPacket = [&](int syncType)
			{
				rl::MessageBuffer mb(1200);

				sync::SyncUnparseState state;
				state.syncType = syncType;
				state.client = client;
				state.buffer = mb;

				bool wroteData = entity->syncTree->Unparse(state);

				if (wroteData)
				{
					cloneBuffer.Write(3, syncType);
					cloneBuffer.Write(13, entity->handle & 0xFFFF);
					cloneBuffer.Write(16, entity->client.lock()->GetNetId()); // TODO: replace with slotId

					if (syncType == 1)
					{
						cloneBuffer.Write(4, (uint8_t)entity->type);
					}

					// TODO: figure out a way to optimize this timestamp
					auto ownerTS = entity->client.lock()->GetData("timestamp");
					auto tgtTS = client->GetData("timestamp");

					if (ownerTS.has_value() && tgtTS.has_value())
					{
						cloneBuffer.Write<uint32_t>(32, entity->timestamp - std::any_cast<int64_t>(ownerTS) + std::any_cast<int64_t>(tgtTS));

						auto len = (state.buffer.GetCurrentBit() / 8) + 1;
						cloneBuffer.Write(12, len);
						cloneBuffer.WriteBits(state.buffer.GetBuffer().data(), len * 8);

						maybeFlushBuffer();

						int slotId = client->GetSlotId();

						if (syncType == 2)
						{
							entity->syncTree->Visit([slotId](sync::NodeBase& node)
							{
								node.ackedPlayers.set(slotId);

								return true;
							});
						}

						((syncType == 1) ? numCreates : numSyncs)++;
					}
				}
				else
				{
					++numSkips;
				}
			};

			if (!hasCreated)
			{
				sendUnparsedPacket(1);
			}
			else
			{
				sendUnparsedPacket(2);
			}
		}

		flushBuffer();

		//trace("%d cr, %d sy, %d sk\n", numCreates, numSyncs, numSkips);
	});
}

void ServerGameState::HandleClientDrop(const std::shared_ptr<fx::Client>& client)
{
	if (!g_oneSyncVar->GetValue())
	{
		return;
	}

	auto clientRegistry = m_instance->GetComponent<fx::ClientRegistry>();

	trace("client drop - reassigning\n");

	std::set<uint32_t> toErase;

	for (auto& entityPair : m_entities)
	{
		auto entity = entityPair.second;

		if (!entity || !entity->syncTree)
		{
			continue;
		}

		bool hasClient = true;

		if (entity->client.expired())
		{
			hasClient = false;
		}
		else if (entity->client.lock()->GetNetId() == client->GetNetId())
		{
			hasClient = false;
		}

		if (!hasClient)
		{
			float posX = entity->GetData("posX", 0.0f);
			float posY = entity->GetData("posY", 0.0f);
			float posZ = entity->GetData("posZ", 0.0f);

			std::vector<std::tuple<float, std::shared_ptr<fx::Client>>> candidates;

			clientRegistry->ForAllClients([&](const std::shared_ptr<fx::Client>& tgtClient)
			{
				if (tgtClient == client)
				{
					return;
				}

				if (tgtClient->GetSlotId() == 0xFFFFFFFF)
				{
					return;
				}

				float distance = std::numeric_limits<float>::max();

				try
				{
					auto entityId = std::any_cast<uint32_t>(client->GetData("playerEntity"));
					auto playerEntity = GetEntity(entityId);

					if (playerEntity)
					{
						float tgtX = entity->GetData("posX", 0.0f);
						float tgtY = entity->GetData("posY", 0.0f);
						float tgtZ = entity->GetData("posZ", 0.0f);

						if (posX != 0.0f)
						{
							float deltaX = (tgtX - posX);
							float deltaY = (tgtY - posY);
							float deltaZ = (tgtZ - posZ);

							distance = (deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ);
						}
					}
				}
				catch (std::bad_any_cast&)
				{
					
				}

				candidates.emplace_back(distance, tgtClient);
			});

			std::sort(candidates.begin(), candidates.end());

			if (entity->type == sync::NetObjEntityType::Player)
			{
				candidates.clear();
			}

			if (candidates.empty())
			{
				trace("no candidates for entity %d, deleting\n", entityPair.first);

				toErase.insert(entityPair.first);
			}
			else
			{
				trace("reassigning entity %d from %s to %s\n", entityPair.first, client->GetName(), std::get<1>(candidates[0])->GetName());

				entity->client = std::get<1>(candidates[0]);
				entity->ackedCreation.set(std::get<1>(candidates[0])->GetSlotId());

				entity->syncTree->Visit([](sync::NodeBase& node)
				{
					node.ackedPlayers.reset();

					return true;
				});
			}
		}
	}

	// here temporarily, needs to be unified with ProcessCloneRemove
	for (auto& set : toErase)
	{
		m_objectIdsUsed.reset(set & 0xFFFF);
		m_entities.erase(set);

		m_instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&](const std::shared_ptr<fx::Client>& thisClient)
		{
			if (thisClient->GetNetId() == client->GetNetId())
			{
				return;
			}

			net::Buffer netBuffer;
			netBuffer.Write<uint32_t>(HashRageString("msgCloneRemove"));
			netBuffer.Write<uint16_t>(set & 0xFFFF);

			thisClient->SendPacket(1, netBuffer, ENET_PACKET_FLAG_RELIABLE);
		});		
	}

	// remove object IDs from sent map
	auto data = GetClientData(this, client);

	for (auto& objectId : data->objectIds)
	{
		m_objectIdsSent.reset(objectId);
	}
}

void ServerGameState::ProcessCloneCreate(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, net::Buffer& ackPacket)
{
	uint16_t objectId = 0;
	ProcessClonePacket(client, inPacket, 1, &objectId);

	m_objectIdsUsed.set(objectId);

	ackPacket.Write<uint8_t>(1);
	ackPacket.Write<uint16_t>(objectId);
}

void ServerGameState::ProcessCloneSync(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, net::Buffer& ackPacket)
{
	ProcessClonePacket(client, inPacket, 2, nullptr);
}

void ServerGameState::ProcessCloneTakeover(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket)
{
	auto clientId = inPacket.Read<uint16_t>(16);
	auto playerId = 0;
	auto objectId = inPacket.Read<uint16_t>(13);

	auto it = m_entities.find(MakeEntityHandle(playerId, objectId));

	if (it != m_entities.end())
	{
		auto tgtCl = (clientId != 0) ? m_instance->GetComponent<fx::ClientRegistry>()->GetClientByNetID(clientId) : client;

		trace("migrating entity %d from %s to %s\n", objectId, it->second->client.lock()->GetName(), tgtCl->GetName());

		auto entity = it->second;

		if (!entity || !entity->syncTree)
		{
			return;
		}

		entity->client = tgtCl;

		auto sourceData = GetClientData(this, client);
		auto targetData = GetClientData(this, tgtCl);

		sourceData->objectIds.erase(objectId);
		targetData->objectIds.insert(objectId);

		entity->syncTree->Visit([](sync::NodeBase& node)
		{
			node.ackedPlayers.reset();

			return true;
		});
	}
}

void ServerGameState::ProcessCloneRemove(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, net::Buffer& ackPacket)
{
	auto playerId = 0;
	auto objectId = inPacket.Read<uint16_t>(13);

	// TODO: verify ownership
	auto entity = m_entities[MakeEntityHandle(playerId, objectId)];

	if (entity)
	{
		if (!entity->client.expired())
		{
			if (client->GetNetId() != entity->client.lock()->GetNetId())
			{
				return;
			}
		}
	}

	m_objectIdsUsed.reset(objectId);
	m_entities.erase(MakeEntityHandle(playerId, objectId));

	// these seem to cause late deletes of state on client, leading to excessive sending of creates
	/*ackPacket.Write<uint8_t>(3);
	ackPacket.Write<uint16_t>(objectId);*/

	m_instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&](const std::shared_ptr<fx::Client>& thisClient)
	{
		if (thisClient->GetNetId() == client->GetNetId())
		{
			return;
		}

		net::Buffer netBuffer;
		netBuffer.Write<uint32_t>(HashRageString("msgCloneRemove"));
		netBuffer.Write<uint16_t>(objectId);

		thisClient->SendPacket(1, netBuffer, ENET_PACKET_FLAG_RELIABLE);
	});
}

void ServerGameState::ProcessClonePacket(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, int parsingType, uint16_t* outObjectId)
{
	auto playerId = 0;
	auto objectId = inPacket.Read<uint16_t>(13);
	//auto objectType = (sync::NetObjEntityType)inPacket.Read<uint8_t>();
	//auto timestamp = inPacket.Read<int32_t>();

	auto objectType = sync::NetObjEntityType::Train;

	if (parsingType == 1)
	{
		objectType = (sync::NetObjEntityType)inPacket.Read<uint8_t>(4);
	}

	auto length = inPacket.Read<uint16_t>(12);

	uint32_t timestamp = 0;

	if (auto tsData = client->GetData("syncTs"); tsData.has_value())
	{
		timestamp = std::any_cast<uint32_t>(tsData);
	}

	if (!client->GetData("timestamp").has_value())
	{
		client->SetData("timestamp", int64_t(timestamp - msec().count()));
	}

	// move this back down under
	client->SetData("playerId", playerId);

	std::vector<uint8_t> bitBytes(length);
	inPacket.ReadBits(&bitBytes[0], bitBytes.size() * 8);

	auto entity = GetEntity(playerId, objectId);

	if (!entity || entity->client.expired())
	{
		if (parsingType == 1)
		{
			entity = std::make_shared<sync::SyncEntityState>();
			entity->client = client;
			entity->type = objectType;
			entity->guid = nullptr;
			entity->handle = MakeEntityHandle(playerId, objectId);

			entity->syncTree = MakeSyncTree(objectType);

			m_entities[MakeEntityHandle(playerId, objectId)] = entity;
		}
		else
		{
			return;
		}
	}

	entity->ackedCreation.set(client->GetSlotId());

	if (entity->client.lock()->GetNetId() != client->GetNetId())
	{
		trace("did object %d migrate? %s isn't %s...\n", objectId, client->GetName(), entity->client.lock()->GetName());

		//entity->client = client;

		return;
	}

	entity->timestamp = timestamp;

	auto state = sync::SyncParseState{ { bitBytes }, parsingType, entity };

	if (entity->syncTree)
	{
		entity->syncTree->Parse(state);

		if (parsingType == 1)
		{
			entity->syncTree->Visit([](sync::NodeBase& node)
			{
				node.ackedPlayers.reset();

				return true;
			});
		}
	}

	switch (entity->type)
	{
		case sync::NetObjEntityType::Player:
			client->SetData("playerEntity", MakeScriptHandle(entity));
			break;
	}

	if (outObjectId)
	{
		*outObjectId = objectId;
	}
}

static std::optional<net::Buffer> UncompressClonePacket(const std::vector<uint8_t>& packetData)
{
	net::Buffer readBuffer(packetData);

	if (readBuffer.Read<uint32_t>() != HashString("netClones"))
	{
		return {};
	}

	uint8_t bufferData[16384] = { 0 };
	int bufferLength = LZ4_decompress_safe(reinterpret_cast<const char*>(&readBuffer.GetData()[4]), reinterpret_cast<char*>(bufferData), readBuffer.GetRemainingBytes(), sizeof(bufferData));

	if (bufferLength <= 0)
	{
		return {};
	}

	return { {bufferData, size_t(bufferLength)} };
}

void ServerGameState::ParseGameStatePacket(const std::shared_ptr<fx::Client>& client, const std::vector<uint8_t>& packetData)
{
	if (!g_oneSyncVar->GetValue())
	{
		return;
	}

	auto packet = UncompressClonePacket(packetData);

	if (!packet)
	{
		return;
	}

	//return;

	auto& buffer = *packet;
	rl::MessageBuffer msgBuf(buffer.GetData().data() + buffer.GetCurOffset(), buffer.GetRemainingBytes());

	auto clientData = GetClientData(this, client);
	
	auto& ackPacket = clientData->ackBuffer;

	if (ackPacket.GetCurOffset() == 0)
	{
		ackPacket.Write(HashRageString("msgCloneAcks"));
	}

	uint32_t numCreates = 0, numSyncs = 0, numRemoves = 0;

	bool end = false;
	
	while (!msgBuf.IsAtEnd() && !end)
	{
		auto dataType = msgBuf.Read<uint8_t>(3);

		switch (dataType)
		{
		case 1: // clone create
			ProcessCloneCreate(client, msgBuf, ackPacket);
			++numCreates;
			break;
		case 2: // clone sync
			ProcessCloneSync(client, msgBuf, ackPacket);
			++numSyncs;
			break;
		case 3: // clone remove
			ProcessCloneRemove(client, msgBuf, ackPacket);
			++numRemoves;
			break;
		case 4: // clone takeover
			ProcessCloneTakeover(client, msgBuf);
			break;
		case 5: // set timestamp
			client->SetData("syncTs", msgBuf.Read<uint32_t>(32));
			break;
		case 7: // end
			end = true;
			break;
		default:
			return;
		}
	}
}

void ServerGameState::SendObjectIds(const std::shared_ptr<fx::Client>& client, int numIds)
{
	// first, gather IDs
	std::vector<int> ids;

	auto data = GetClientData(this, client);

	int id = 1;

	for (int i = 0; i < numIds; i++)
	{
		bool hadId = false;

		for (; id < m_objectIdsSent.size(); id++)
		{
			if (!m_objectIdsSent.test(id) && !m_objectIdsUsed.test(id))
			{
				hadId = true;

				data->objectIds.insert(id);

				ids.push_back(id);
				m_objectIdsSent.set(id);

				break;
			}
		}

		if (!hadId)
		{
			trace("couldn't assign an object id for player!\n");
			break;
		}
	}

	// compress and send

	// adapted from https://stackoverflow.com/a/1081776
	std::vector<std::tuple<int, int>> pairs;

	int last = -1, len = 0;

	for (int i = 0; i < ids.size(); )
	{
		int gap = ids[i] - 2 - last;
		int size = 0;

		while (++i < ids.size() && ids[i] == ids[i - 1] + 1) size++;

		last = ids[i - 1];

		pairs.emplace_back(gap, size);
	}

	net::Buffer outBuffer;
	outBuffer.Write<uint32_t>(HashRageString("msgObjectIds"));
	outBuffer.Write<uint16_t>(pairs.size());

	for (auto& pair : pairs)
	{
		auto [gap, size] = pair;

		outBuffer.Write<uint16_t>(gap);
		outBuffer.Write<uint16_t>(size);
	}

	client->SendPacket(1, outBuffer, ENET_PACKET_FLAG_RELIABLE);
}

void ServerGameState::AttachToObject(fx::ServerInstanceBase* instance)
{
	m_instance = instance;
}
}

#include <ResourceManager.h>
#include <ServerInstanceBase.h>
#include <ServerInstanceBaseRef.h>
#include <ScriptEngine.h>

static InitFunction initFunction([]()
{
	g_scriptHandlePool = new CPool<fx::ScriptGuid>(1500, "fx::ScriptGuid");

	auto makeEntityFunction = [](auto fn, uintptr_t defaultValue = 0)
	{
		return [=](fx::ScriptContext& context)
		{
			// get the current resource manager
			auto resourceManager = fx::ResourceManager::GetCurrent();

			// get the owning server instance
			auto instance = resourceManager->GetComponent<fx::ServerInstanceBaseRef>()->Get();

			// get the server's game state
			auto gameState = instance->GetComponent<fx::ServerGameState>();

			// parse the client ID
			auto id = context.GetArgument<uint32_t>(0);

			if (!id)
			{
				context.SetResult(defaultValue);
				return;
			}

			auto entity = gameState->GetEntity(id);

			if (!entity)
			{
				trace("Tried to access invalid entity.\n");

				context.SetResult(defaultValue);
				return;
			}

			context.SetResult(fn(context, entity));
		};
	};

	fx::ScriptEngine::RegisterNativeHandler("GET_ENTITY_COORDS", makeEntityFunction([](fx::ScriptContext& context, const std::shared_ptr<fx::sync::SyncEntityState>& entity)
	{
		struct scrVector
		{
			float x;
			int pad;
			float y;
			int pad2;
			float z;
			int pad3;
		};

		scrVector resultVec = { 0 };
		resultVec.x = entity->GetData("posX", 0.0f);
		resultVec.y = entity->GetData("posY", 0.0f);
		resultVec.z = entity->GetData("posZ", 0.0f);

		return resultVec;
	}));

	fx::ScriptEngine::RegisterNativeHandler("GET_HASH_KEY", [](fx::ScriptContext& context)
	{
		context.SetResult(HashString(context.GetArgument<const char*>(0)));
	});

	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		g_oneSyncVar = instance->AddVariable<bool>("onesync_enabled", ConVar_ServerInfo, false);

		instance->SetComponent(new fx::ServerGameState);

		instance->GetComponent<fx::GameServer>()->OnTick.Connect([=]()
		{
			if (!g_oneSyncVar->GetValue())
			{
				return;
			}

			instance->GetComponent<fx::ServerGameState>()->Tick(instance);
		});

		auto gameServer = instance->GetComponent<fx::GameServer>();

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("msgNetGameEvent"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			auto targetPlayerCount = buffer.Read<uint8_t>();
			std::vector<uint16_t> targetPlayers(targetPlayerCount);

			if (!buffer.Read(targetPlayers.data(), targetPlayers.size() * sizeof(uint16_t)))
			{
				return;
			}

			net::Buffer netBuffer;
			netBuffer.Write<uint32_t>(HashRageString("msgNetGameEvent"));
			netBuffer.Write<uint16_t>(client->GetNetId());
			buffer.ReadTo(netBuffer, buffer.GetRemainingBytes());

			auto clientRegistry = instance->GetComponent<fx::ClientRegistry>();

			for (uint16_t player : targetPlayers)
			{
				auto targetClient = clientRegistry->GetClientByNetID(player);

				if (targetClient)
				{
					targetClient->SendPacket(1, netBuffer, ENET_PACKET_FLAG_RELIABLE);
				}
			}
		});

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("msgRequestObjectIds"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			instance->GetComponent<fx::ServerGameState>()->SendObjectIds(client, 32);
		});

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("ccack"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			auto entity = instance->GetComponent<fx::ServerGameState>()->m_entities[fx::MakeEntityHandle(0, buffer.Read<uint16_t>())];

			if (entity && entity->syncTree)
			{
				entity->syncTree->Visit([client](fx::sync::NodeBase& node)
				{
					node.ackedPlayers.set(client->GetSlotId());

					return true;
				});

				entity->ackedCreation.set(client->GetSlotId());
			}
		});

		// TODO: handle this based on specific nodes sent with a specific ack
		/*gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("csack"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			auto entity = instance->GetComponent<fx::ServerGameState>()->m_entities[fx::MakeEntityHandle(0, buffer.Read<uint16_t>())];

			if (entity)
			{
				auto slotId = client->GetSlotId();

				entity->syncTree->Visit([slotId](fx::sync::NodeBase& node)
				{
					node.ackedPlayers.set(slotId);

					return true;
				});
			}
		});*/
	}, 999999);
});
