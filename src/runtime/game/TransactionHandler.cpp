#include "../util/HookingUtil.hpp"
#include "websocket/WebsocketCore.hpp"
#include "TransactionHandler.hpp"
#include "structures/PointerWrapper.hpp"
#include <Logger.hpp>
#include <mutex>

#include "Menu.hpp"

struct Steamworks_GameOverlayActivated_t
{
	uint8_t m_bActive;
	bool m_bUserInitiated;
	uint32_t m_AppId;
};

struct Steamworks_MicroTxnAuthorizationResponse_t
{
	uint32_t m_unAppID;
	uint64_t m_ulOrderID;
	uint8_t m_bAuthorized;
};

struct Cysharp_Threading_Tasks_CompilerServices_IStateMachineRunner
{
	void* klass;
	void* monitor;
	void* fields;
};

struct Cysharp_Threading_Tasks_UniTask_Awaiter_o
{
	void* source;
	int16_t token;
};

struct Cysharp_Threading_Tasks_UniTask_Awaiter_int__o
{
	void* source;
	IL2CPP::Object* result;
	int16_t token;
};

struct PurchaseCore_OverlayCallbackTask 
{
	int32_t state;
	Cysharp_Threading_Tasks_CompilerServices_IStateMachineRunner* runner;
	IL2CPP::Object* thiz;
	IL2CPP::Object* product;
	IL2CPP::Object* idfk;
	IL2CPP::String* steamId;
	IL2CPP::String* languageCode;
	IL2CPP::String* productId;
	IL2CPP::String* description;
	int32_t serverPrice;
	IL2CPP::Object* initTransactionCallback;
	IL2CPP::Object* overlayCallback;
	IL2CPP::Object* microtransactionCallback;
	IL2CPP::Object* finalizeTransactionCallback;
	Cysharp_Threading_Tasks_UniTask_Awaiter_o awaiter1;
	Cysharp_Threading_Tasks_UniTask_Awaiter_int__o awaiter2;
};

static std::string gIapTransactionId;
static uint64_t gIapOrderId;
static IL2CPP::Wrapper::Method<void(IL2CPP::Object*, Steamworks_MicroTxnAuthorizationResponse_t)> PurchaseCore_TxnAuthorizationResponseCallback;

bool gSpoofIAPSocket = false;

static void SocketReceive_steam_finalize_transaction(nlohmann::json& response)
{
	if (!Menu::Misc::Bypass::Misc::MicrotransactionSpoofer.IsActive())
	{
		return;
	}

	try
	{
		LOG_INFO("Spoofing steam_finalize_transaction response.");
		int reqId = response.at("req_id");

		response = nlohmann::json({
			{"status", "ok"},
			{"response", 1},
			{"req_id", reqId},
			{"success", true }
		});
	}
	catch(std::exception& err)
	{ 
		LOG_ERROR("Exception throw when spoofing steam_finalize_transaction response: %s", err.what());
	}
}

static void SocketReceive_steam_init_transaction(nlohmann::json& response)
{
	if (!Menu::Misc::Bypass::Misc::MicrotransactionSpoofer.IsActive())
	{
		return;
	}

	try
	{
		gIapOrderId = response.at("order_id");
		gIapTransactionId = response.at("transaction_id");

		LOG_INFO("order_id: %i", gIapOrderId);
		LOG_INFO("transaction_id: %s", gIapTransactionId.c_str());
	}
	catch(std::exception& err)
	{ 
		LOG_ERROR("Exception throw when handling transaction: %s", err.what());
	}
}

static void SocketSend_update_progress(nlohmann::json& response)
{
	try
	{
		if (!Menu::Misc::Bypass::Misc::MicrotransactionSpoofer.IsActive())
		{
			return;
		}

		if (!gSpoofIAPSocket)
		{
			return;
		}

		auto& responseId = response.at("id");
		if (!responseId.is_number() || responseId != CommandID::Snapshot)
		{
			return;
		}

		auto& commands = response.at("p").at("c");
		if (!commands.is_array())
		{
			return;
		}

		LOG_INFO("Fixing MicroTxn progress commands.");

		bool removableFound = false;
		for (auto it = commands.begin(); it != commands.end(); )
		{
			if (!it->is_object())
			{
				it = commands.erase(it);
				return;
			}

			auto& id = it->at("id");
			const bool removable = id.is_number() && (
				id == CommandID::ClanStorePromotionSendGenerateData ||
				id == CommandID::InappTransactionAddTransaction ||
				id == CommandID::ClanStorePromotionSave || 
				id == CommandID::ValidateInApp
			);

			if (removable)
			{
				it = commands.erase(it);
				removableFound = true;
				continue;
			}

			++it;
		}

		if (removableFound)
		{
			gSpoofIAPSocket = false;
		}
	}
	catch (std::exception& err)
	{	
		LOG_ERROR("Exception throw when handling transaction socket: %s", err.what());
	}
}

$Hook(void, PurchaseCore_OverlayCallbackTask_Hook, (PurchaseCore_OverlayCallbackTask* _this))
{
	if (!Menu::Misc::Bypass::Misc::MicrotransactionSpoofer.IsActive())
	{
		$CallOrig(PurchaseCore_OverlayCallbackTask_Hook, _this);
		return;
	}

	if (_this->state == 3)
	{
		LOG_INFO("Calling MicroTxn callback", _this->state);

		gSpoofIAPSocket = true;
		const auto gClass = _this->microtransactionCallback->GetClass();
		const auto OnRunCallback_info = gClass->GetMethod("OnRunCallback");
		IL2CPP::Wrapper::Method<void(IL2CPP::Object*, void*, void*)> OnRunCallback = OnRunCallback_info;

		void* pResponse = new Steamworks_MicroTxnAuthorizationResponse_t
		{
			2524890,
			gIapOrderId,
			true
		};

		OnRunCallback(
			_this->microtransactionCallback,
			pResponse,
			OnRunCallback_info
		);

		_this->state = 4;
	}

	$CallOrig(PurchaseCore_OverlayCallbackTask_Hook, _this);
}

void TransactionHandler::INIT()
{
	const auto purchaseCore_class = IL2CPP::ClassMapping::GetClass("PurchaseCore");
	const auto purchaseCore_overlayTask_class = IL2CPP::ClassMapping::GetClass("PurchaseCore_OverlayTask");
	const auto purchaseCore_overlayTask_overlayCallbackTask_info = purchaseCore_overlayTask_class->GetMethod(0x0);

	PurchaseCore_TxnAuthorizationResponseCallback = purchaseCore_class->GetMethod(0x4);

	WebsocketCore::SubscribeReceiveEvent("steam_init_transaction", SocketReceive_steam_init_transaction);
	WebsocketCore::SubscribeReceiveEvent("steam_finalize_transaction", SocketReceive_steam_finalize_transaction);
	WebsocketCore::SubscribeSendEvent("update_progress", SocketSend_update_progress);

	$RegisterHook(
		PurchaseCore_OverlayCallbackTask_Hook,
		purchaseCore_overlayTask_overlayCallbackTask_info
	);
}