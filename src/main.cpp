namespace
{
	constexpr std::string_view kTongueRoot = "tong1 1"sv;  // compare sur la FIN du nom (HDT-SMP renomme les bones d'armure)

	struct Seen { RE::NiAVObject* root{}; RE::NiAVObject* tongue{}; };
	std::unordered_map<RE::FormID, Seen> g_seen;
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

	RE::NiAVObject* FindTongue(RE::NiAVObject* o)
	{
		if (!o) return nullptr;
		auto n = Name(o);
		if (n.size() >= kTongueRoot.size() && n.substr(n.size() - kTongueRoot.size()) == kTongueRoot) return o;
		if (auto* node = o->AsNode()) {
			for (auto& c : node->GetChildren()) {
				if (auto* f = FindTongue(c.get()))
					return f;
			}
		}
		return nullptr;
	}

	// Une ligne de log par shape interessante. face = true si une shape est sous le noeud FaceGen.
	void Collect(RE::NiAVObject* o, bool underFace, std::vector<std::string>& lines, bool& face)
	{
		if (!o) return;
		underFace = underFace || Has(Name(o), "facegen");
		if (auto* g = o->AsGeometry(); g && (underFace || Has(Name(o), "tongue") || Has(Name(o), "mouth") || Has(Name(o), "teeth"))) {
			face = face || underFace;
			auto* tri = netimmerse_cast<RE::BSTriShape*>(g);
			auto* dyn = netimmerse_cast<RE::BSDynamicTriShape*>(g);
			const auto n = tri ? tri->GetTrishapeRuntimeData().vertexCount : 0u;
			const auto* v = dyn ? static_cast<const RE::NiPoint4*>(dyn->GetDynamicTrishapeRuntimeData().dynamicData) : nullptr;
			if (v && n) {
				std::vector<float> z;
				float mn[3]{ 1e30f, 1e30f, 1e30f }, mx[3]{ -1e30f, -1e30f, -1e30f };
				double sum[3]{};
				for (std::uint32_t i = 0; i < n; ++i) {
					const float p[3]{ v[i].x, v[i].y, v[i].z };
					for (int k = 0; k < 3; ++k) { mn[k] = (std::min)(mn[k], p[k]); mx[k] = (std::max)(mx[k], p[k]); sum[k] += p[k]; }
					z.push_back(p[2]);
				}
				auto mid = z.begin() + z.size() / 2;
				std::nth_element(z.begin(), mid, z.end());
				double up = 0; int nu = 0;
				for (float f : z) if (f >= *mid) { up += f; ++nu; }
				lines.push_back(std::format("  {} \"{}\" DYNAMIC verts={} bbox=({:.2f},{:.2f},{:.2f})..({:.2f},{:.2f},{:.2f}) centroid=({:.2f},{:.2f},{:.2f}) upperMeanZ={:.2f}",
					underFace ? "face" : "else", Name(o), n, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], sum[0] / n, sum[1] / n, sum[2] / n, up / nu));
			}
			else {
				lines.push_back(std::format("  {} \"{}\" static verts={}", underFace ? "face" : "else", Name(o), n));
			}
		}
		if (auto* node = o->AsNode()) {
			for (auto& c : node->GetChildren()) Collect(c.get(), underFace, lines, face);
		}
	}

	void Process(RE::Actor* a)
	{
		auto* root = a ? a->Get3D() : nullptr;
		if (!root) return;
		auto* tongue = FindTongue(root);
		auto& s = g_seen[a->GetFormID()];
		if (s.root == root && s.tongue == tongue) return;

		std::vector<std::string> lines;
		bool face = false;
		Collect(root, false, lines, face);
		if (!face) return;

		s = { root, tongue };
		const auto* race = a->GetRace();
		const char* edid = race ? race->GetFormEditorID() : nullptr;
		SKSE::log::info("==== {} [{:08X}] race={} tongueRoot={}", a->GetName(), a->GetFormID(), edid && *edid ? edid : "?", tongue ? "FOUND" : "none");
		if (tongue) {
			SKSE::log::info("  tongue root local.t=({:.3f},{:.3f},{:.3f})", tongue->local.translate.x, tongue->local.translate.y, tongue->local.translate.z);
		}
		for (auto& l : lines) SKSE::log::info("{}", l);
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
	SKSE::log::info("Data loaded TongueFix");

	g_ticker = std::jthread([](std::stop_token st) {
		while (!st.stop_requested()) {
			std::this_thread::sleep_for(2s);
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

	SKSE::log::info("Start TongueFix");

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}

	return true;
}