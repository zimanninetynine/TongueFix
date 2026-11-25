namespace
{
	constexpr std::string_view kTongueRoot = "tong1 1"sv;  // compare sur la FIN du nom (HDT-SMP renomme les bones d'armure)
	constexpr std::string_view kHeadNode = "NPC Head [Head]"sv;
	constexpr std::string_view kHelperName = "TongueFixHead"sv;  // noeud d'aide cree sous la tete de chaque acteur

	constexpr float kOffsetZ = 2.5f;  // + = monte, - = descend
	constexpr float kOffsetY = 1.2f;  // + = avance (vers l'exterieur de la bouche), - = recule (vers la gorge)

	constexpr int kMaxTries = 200;  // scans pendant lesquels on attend que la shape de la langue soit attachee

	struct State
	{
		RE::NiAVObject* root{};
		RE::NiAVObject* tongue{};
		float baseY{}, baseZ{};
		bool done{ false };  // skin rattachee (ou abandon apres kMaxTries)
		int tries{ 0 };
	};
	std::unordered_map<RE::FormID, State> g_state;
	std::jthread g_ticker;
	std::atomic_bool g_queued{ false };

	std::string_view Name(const RE::NiAVObject* o)
	{
		const char* c = o ? o->name.c_str() : nullptr;
		return c ? c : "";
	}

	bool Has(std::string_view s, std::string_view part)
	{
		auto it = std::search(s.begin(), s.end(), part.begin(), part.end(), [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
		return it != s.end();
	}

	// Premier noeud dont le nom FINIT par suffix
	RE::NiAVObject* FindNode(RE::NiAVObject* o, std::string_view suffix)
	{
		if (!o) return nullptr;
		auto n = Name(o);
		if (n.size() >= suffix.size() && n.substr(n.size() - suffix.size()) == suffix) return o;
		if (auto* node = o->AsNode()) {
			for (auto& c : node->GetChildren()) {
				if (auto* f = FindNode(c.get(), suffix))
					return f;
			}
		}
		return nullptr;
	}

	// Noeud d'aide : enfant de la tete, meme rotation et echelle qu'elle (rotation identite, echelle 1), seule sa translation change.
	RE::NiNode* GetHelper(RE::NiAVObject* head)
	{
		auto* headNode = head ? head->AsNode() : nullptr;
		if (!headNode) return nullptr;
		for (auto& c : headNode->GetChildren()) {
			if (c && Name(c.get()) == kHelperName) return c->AsNode();
		}
		auto* h = RE::NiNode::Create(0);
		if (!h) return nullptr;
		h->name = kHelperName.data();
		h->local.scale = 1.0f;
		h->local.translate = { 0.0f, kOffsetY, kOffsetZ };
		h->world = head->world * h->local;  // valide des la premiere frame : pas de clignotement a l'origine
		headNode->AttachChild(h, true);
		return h;
	}

	// Pour chaque shape dont la skin contient un bone "tong1" : remplace l'entree "NPC Head [Head]" par le noeud d'aide
	// (bones[] ET boneWorldTransforms[]).
	void Rebind(RE::NiAVObject* o, int& rebound)
	{
		if (!o) return;
		if (auto* g = o->AsGeometry()) {
			auto* skin = g->GetGeometryRuntimeData().skinInstance.get();
			if (skin && skin->bones) {
				bool tong = false;
				for (std::uint32_t i = 0; i < skin->numMatrices; ++i) {
					if (Has(Name(skin->bones[i]), "tong1")) tong = true;
				}
				if (tong) {
					for (std::uint32_t i = 0; i < skin->numMatrices; ++i) {
						auto* b = skin->bones[i];
						if (b && Name(b) == kHeadNode) {
							if (auto* h = GetHelper(b)) {
								skin->bones[i] = h;
								skin->boneWorldTransforms[i] = &h->world;
								++rebound;
							}
						}
					}
				}
			}
		}
		if (auto* node = o->AsNode()) {
			for (auto& c : node->GetChildren()) Rebind(c.get(), rebound);
		}
	}

	void Process(RE::Actor* a)
	{
		if (!a) return;
		auto* root = a->Get3D();
		if (!root) return;
		auto* tongue = FindNode(root, kTongueRoot);
		if (!tongue) {
			g_state.erase(a->GetFormID());  // plus de langue : on repartira de zero si elle revient
			return;
		}

		auto& s = g_state[a->GetFormID()];
		if (s.root != root || s.tongue != tongue) {  // nouvelle langue (ou nouveau 3D) : memorise la position d'origine du bone
			s = State{};
			s.root = root;
			s.tongue = tongue;
			s.baseY = tongue->local.translate.y;
			s.baseZ = tongue->local.translate.z;
			SKSE::log::info("Langue trouvee sur {} [{:08X}] : origine Y={:.3f} Z={:.3f}", a->GetName(), a->GetFormID(), s.baseY, s.baseZ);
		}

		tongue->local.translate.y = s.baseY + kOffsetY;  // moitie avant : la chaine tong1
		tongue->local.translate.z = s.baseZ + kOffsetZ;

		if (!s.done) {  // moitie arriere : les vertices attaches a la tete. On reessaie tant que la shape n'est pas attachee.
			int rebound = 0;
			Rebind(root, rebound);
			if (rebound) {
				s.done = true;
				SKSE::log::info("  {} : skin rattachee au noeud d'aide ({} entree(s))", a->GetName(), rebound);
			}
			else if (++s.tries >= kMaxTries) {
				s.done = true;
				SKSE::log::info("  {} : aucune shape de langue trouvee apres {} scans, abandon", a->GetName(), kMaxTries);
			}
		}
	}

	void Poll()
	{
		if (auto* pc = RE::PlayerCharacter::GetSingleton()) Process(pc);
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			for (auto& h : lists->highActorHandles) {
				if (auto a = h.get())
				{
					Process(a.get());
				}
			}
		}
		g_queued = false;
	}
}

void OnDataLoaded()
{
	SKSE::log::info("Init TongueFix");

	g_ticker = std::jthread([](std::stop_token st) {
		while (!st.stop_requested()) {
			std::this_thread::sleep_for(500ms);
			if (!g_queued.exchange(true)) SKSE::GetTaskInterface()->AddTask([] {
				Poll();
				});
		}
		});
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		OnDataLoaded();
		break;
	case SKSE::MessagingInterface::kPostLoad:
		break;
	case SKSE::MessagingInterface::kPreLoadGame:
		break;
	case SKSE::MessagingInterface::kPostLoadGame:
		break;
	case SKSE::MessagingInterface::kNewGame:
		break;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}

	return true;
}