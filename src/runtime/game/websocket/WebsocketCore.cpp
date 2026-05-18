#include "WebsocketCore.hpp"
#include <IL2CPP.hpp>
#include <utility>
#include <Logger.hpp>
#include <unordered_set>

#include "MessageBuilder.hpp"
#include "../import/PointerFunctions.hpp"
#include "../Functions.hpp"
#include "../../util/HookingUtil.hpp"

namespace WebsocketCore
{
	using nlohmann::json;

	struct SocketQueue
	{
		const int requestId;
		const SocketCallback onRecieve;
		const json package;

		SocketQueue(int requestId, SocketCallback onRecieve, const json& package) :
			requestId(requestId),
			onRecieve(onRecieve),
			package(package)
		{}
	};

	bool logWebsocket = false;
	IL2CPP::Object* WSManagerInstance = nullptr;
	std::vector<SocketQueue*> commandQueue;
	std::vector<SocketQueue*> responseQueue;
	std::unordered_map<std::string, std::unordered_set<SocketCallback>> sendSubscriber;
	std::unordered_map<std::string, std::unordered_set<SocketCallback>> receiveSubscriber;

	void QueuePackage(const std::string& msgType, const json& package, SocketCallback onReceive)
	{
		assert(WSManagerInstance && "Queuing WebSocket package when WebSocketManager is not initialized.");

		int requestId = WSManagerInstance->GetFieldRef<int>(0x4);
		commandQueue.push_back(new SocketQueue(
			requestId,
			onReceive,
			package
		));

		WebSocketManager::SendEvent(WSManagerInstance, IL2CPP::String::Create(msgType));
	}

	void SubscribeSendEvent(const std::string_view& event, SocketCallback onSend)
	{
		const auto eventKey = std::string(event);
		if (const auto subsSetIt = sendSubscriber.find(eventKey); subsSetIt != sendSubscriber.end())
		{
			subsSetIt->second.insert(onSend);
		}
		else
		{
			sendSubscriber.insert({eventKey, {onSend}});
		}
	}

	void SubscribeReceiveEvent(const std::string_view& event, SocketCallback onReceive)
	{
		const auto eventKey = std::string(event);
		if (const auto subsSetIt = receiveSubscriber.find(eventKey); subsSetIt != receiveSubscriber.end())
		{
			subsSetIt->second.insert(onReceive);
		}
		else
		{
			receiveSubscriber.insert({eventKey, {onReceive}});
		}
	}

	inline int PendingPackageCount()
	{
		return commandQueue.size();
	}

	inline int PendingReceiverCount()
	{
		return responseQueue.size();
	}

	void ClearQueue()
	{
		commandQueue.clear();
		responseQueue.clear();
	}

	void Reload()
	{
		QueuePackage("update_progress", json::object({}));
	}

	$Hook(int, SendSocketMessage, (IL2CPP::Object* _this, IL2CPP::String* messageType, IL2CPP::Object* jsonObject))
	{
		if (WSManagerInstance == nullptr)
		{
			WSManagerInstance = _this;
		}

		return $CallOrig(SendSocketMessage, _this, messageType, jsonObject);
	}

	$Hook(IL2CPP::Array<char>*, SocketReceive, (IL2CPP::Object* _this, IL2CPP::Array<char>* rawBuffer))
	{
		using namespace nlohmann;
		IL2CPP::Array<char>* buffer = $CallOrig(SocketReceive, _this, rawBuffer);

		if (buffer == nullptr)
		{
			return buffer;
		}

		static std::string command = "";
		std::string data{ buffer->GetVectorPointer(), buffer->GetVectorPointer() + buffer->GetLength() };

		if (buffer->Get(0) == '{')
		{
			int reqId = 0;
			json parsedData = json::parse(data);

			if (parsedData.contains("req_id"))
			{
				reqId = parsedData["req_id"];
			}

			if (const auto& eventSet = receiveSubscriber.find(command); eventSet != receiveSubscriber.end())
			{
				for (const auto& subscriber : eventSet->second)
				{
					subscriber(parsedData);
				}
			}

			if (!responseQueue.empty())
			{
				auto it = responseQueue.begin();
				while (it != responseQueue.end())
				{
					SocketQueue* queue = *it._Ptr;
					if (queue->requestId != reqId)
					{
						++it;
					}
					else
					{
						parsedData.update(queue->package, true);
						if (queue->onRecieve != nullptr)
						{
							queue->onRecieve(parsedData);
						}
						responseQueue.erase(it);
						delete queue;
						break;
					}
				}
			}

			if (logWebsocket)
			{
				LOG_NOTAG("[Receive | %s] \n%s\n", command.c_str(), parsedData.dump().c_str());
			}

			buffer = Encoding::GetBytes(Encoding::GetUTF8(), IL2CPP::String::Create(parsedData.dump()));
		}
		else
		{
			command = data;
			std::erase(command, '\"');
		}

		return buffer;
	}

	$Hook(IL2CPP::Array<char>*, SocketSend, (IL2CPP::Object* _this, IL2CPP::Array<char>* buffer, bool idfk))
	{
		using namespace nlohmann;
		if (buffer == nullptr)
		{
			return $CallOrig(SocketSend, _this, buffer, idfk);
		}

		static std::string command = "";
		std::string data{ buffer->GetVectorPointer(), buffer->GetVectorPointer() + buffer->GetLength() };

		if (buffer->Get(0) == '{')
		{
			int reqId = -1;

			json parsedData = json::parse(data);
			if (parsedData.contains("req_id"))
			{
				reqId = parsedData["req_id"];
			}

			if (const auto& eventSet = sendSubscriber.find(command); eventSet != sendSubscriber.end())
			{
				for (const auto& subscriber : eventSet->second)
				{
					subscriber(parsedData);
				}
			}

			if (!commandQueue.empty())
			{
				auto it = commandQueue.begin();
				while (it != commandQueue.end())
				{
					SocketQueue* queue = *it._Ptr;
					if (queue->requestId != reqId)
					{
						++it;
					}
					else
					{
						parsedData.update(queue->package, true);
						responseQueue.push_back(queue);
						commandQueue.erase(it);
						break;
					}
				}
			}

			if (logWebsocket)
			{
				LOG_NOTAG("[Send | %s] \n%s\n", command.c_str(), parsedData.dump().c_str());
			}

			buffer = Encoding::GetBytes(Encoding::GetUTF8(), IL2CPP::String::Create(parsedData.dump()));
		}
		else
		{
			command = data;
			std::erase(command, '\"');
		}

		return $CallOrig(SocketSend, _this, buffer, idfk);
	}

	void INIT()
	{
		using namespace IL2CPP::ClassMapping;

		$RegisterHook(
			SendSocketMessage, 
			WebSocketManager::Send.GetPointer()
		);

		$RegisterHook(
			SocketSend, 
			GetClass("AesFacade")->GetMethod(0x2)
		);

		$RegisterHook(
			SocketReceive,
			GetClass("AesFacade")->GetMethod(0x1)
		);
	}

	void ISocketHelper::SendPackage(SocketCallback onRecieve)
	{
		QueuePackage(this->eventType, this->package, onRecieve);
	}

	ProgressUpdaterHelper::ProgressUpdaterHelper()
	{
		this->eventType = "update_progress";
		this->package = json::array({});
	}

	void ProgressUpdaterHelper::IterateRegisterKey(OfferItemType itemType, CommandID cmdId, 
													const std::function<std::optional<json::object_t>(std::string, int)>& iterator)
	{
		ContentKeyRegister::IterateKeyRegister(itemType, [&](IL2CPP::String* x, int i)
		{
			std::optional<json> out = iterator(x->ToString(), i);

			if (out.has_value())
			{
				this->package.push_back(MessageBuilder::BuildCommand(cmdId, out.value()));
			}
		});
	}

	void ProgressUpdaterHelper::IterateRegisterKey(OfferItemType itemType, CommandID cmdId, size_t from, size_t to, 
													const std::function<std::optional<json::object_t>(std::string, int)>& iterator)
	{
		ContentKeyRegister::IterateKeyRegister(from, to, itemType, [&](IL2CPP::String* x, int i)
		{
			std::optional<json> out = iterator(x->ToString(), i);

			if (out.has_value())
			{
				this->package.push_back(MessageBuilder::BuildCommand(cmdId, out.value()));
			}
		});
	}

	void ProgressUpdaterHelper::AddCommand(CommandID cmdId, const json& json_)
	{
		this->package.push_back(MessageBuilder::BuildCommand(cmdId, json_));
	}

	void ProgressUpdaterHelper::AddCommand(CommandID cmdId, int headerHexLength, const json& json_)
	{
		this->package.push_back(MessageBuilder::BuildCommand(cmdId, headerHexLength, json_));
	}

	void ProgressUpdaterHelper::SendPackage(SocketCallback onRecieve)
	{
		QueuePackage(this->eventType, MessageBuilder::BuildSnapshot(this->package), onRecieve);
	}
}