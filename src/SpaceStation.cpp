#include "SpaceStation.h"
#include "Ship.h"
#include "Planet.h"
#include "ModelCollMeshData.h"
#include "gameconsts.h"
#include "StarSystem.h"
#include "Serializer.h"
#include "Frame.h"
#include "Pi.h"
#include "Mission.h"
#include "CityOnPlanet.h"
#include "Shader.h"

struct SpaceStationType {
	const char *sbreModelName;
	enum { ORBITAL, SURFACE } dockMethod;
};

struct SpaceStationType stationTypes[SpaceStation::TYPE_MAX] = {
	{ "nice_spacestation", SpaceStationType::ORBITAL },
	{ "basic_groundstation", SpaceStationType::SURFACE },
};

void SpaceStation::Save()
{
	using namespace Serializer::Write;
	ModelBody::Save();
	MarketAgent::Save();
	wr_int((int)m_type);
	wr_float(m_doorsOpen);
	wr_float(m_playerDockingTimeout);
	for (int i=0; i<Equip::TYPE_MAX; i++) {
		wr_int((int)m_equipmentStock[i]);
	}
	// save shipyard
	wr_int(m_shipsOnSale.size());
	for (std::vector<ShipFlavour>::iterator i = m_shipsOnSale.begin();
			i != m_shipsOnSale.end(); ++i) {
		(*i).Save();
	}
	// save bb missions
	wr_int(m_bbmissions.size());
	for (std::vector<Mission*>::iterator i = m_bbmissions.begin();
			i != m_bbmissions.end(); ++i) {
		(*i)->Save();
	}
	for (int i=0; i<MAX_DOCKING_PORTS; i++) {
		wr_int(Serializer::LookupBody(m_shipDocking[i].ship));
		wr_int(m_shipDocking[i].stage);
		wr_float(m_shipDocking[i].stagePos);
		wr_vector3d(m_shipDocking[i].from);
	}
	wr_double(m_lastUpdatedShipyard);
	wr_int(Serializer::LookupSystemBody(m_sbody));
}

void SpaceStation::Load()
{
	using namespace Serializer::Read;
	ModelBody::Load();
	MarketAgent::Load();
	m_type = (TYPE)rd_int();
	m_doorsOpen = rd_float();
	m_playerDockingTimeout = rd_float();
	m_numPorts = 0;
	for (int i=0; i<Equip::TYPE_MAX; i++) {
		m_equipmentStock[i] = static_cast<Equip::Type>(rd_int());
	}
	// load shityard
	int numShipsForSale = rd_int();
	for (int i=0; i<numShipsForSale; i++) {
		ShipFlavour s;
		s.Load();
		m_shipsOnSale.push_back(s);
	}
	// load bbmissions
	int numBBMissions = rd_int();
	for (int i=0; i<numBBMissions; i++) {
		Mission *m = Mission::Load();
		m_bbmissions.push_back(m);
	}
	for (int i=0; i<MAX_DOCKING_PORTS; i++) {
		m_shipDocking[i].ship = (Ship*)rd_int();
		m_shipDocking[i].stage = rd_int();
		m_shipDocking[i].stagePos = rd_float();
		m_shipDocking[i].from = rd_vector3d();
	}
	m_lastUpdatedShipyard = rd_double();
	m_sbody = Serializer::LookupSystemBody(rd_int());
	Init();
}

void SpaceStation::PostLoadFixup()
{
	for (int i=0; i<MAX_DOCKING_PORTS; i++) {
		m_shipDocking[i].ship = static_cast<Ship*>(Serializer::LookupBody((size_t)m_shipDocking[i].ship));
		printf("post-load[%d]: %p\n", i, m_shipDocking[i].ship);
	}
}

static ObjParams params = {
	{ 0.5, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f },

	{	// pColor[3]
	{ { 1.0f, 0.0f, 1.0f }, { 0, 0, 0 }, { 0, 0, 0 }, 0 },
	{ { 0.8f, 0.6f, 0.5f }, { 0, 0, 0 }, { 0, 0, 0 }, 0 },
	{ { 0.5f, 0.5f, 0.5f }, { 0, 0, 0 }, { 0, 0, 0 }, 0 } },

	// pText[3][256]	
	{ "Hello old bean", "DIET STEAKETTE" },
};

SpaceStation::SpaceStation(const SBody *sbody): ModelBody()
{
	if (sbody->type == SBody::TYPE_STARPORT_ORBITAL) {
		m_type = JJHOOP;
	} else {
		m_type = GROUND_FLAVOURED;
	}
	m_doorsOpen = 0;
	m_playerDockingTimeout = 0;
	m_sbody = sbody;
	m_numPorts = 0;
	m_lastUpdatedShipyard = 0;
	for (int i=1; i<Equip::TYPE_MAX; i++) {
		m_equipmentStock[i] = Pi::rng.Int32(0,100);
	}
	for (int i=0; i<MAX_DOCKING_PORTS; i++) m_shipDocking[i].ship = 0;
	SetMoney(1000000000);
	Init();
}

/*
 * So, positionOrient_t things for docking animations are stored in the model
 * as two triangles with geomflag 0x8000+i (0x8010+i for stage2)
 * tri(position, zerovec, zerovec)
 * tri(zerovec, xaxis, yaxis)
 * Since positionOrient triangles in subobjects will be scaled and translated,
 * the xaxis and yaxis normals must be un-translated (xaxis minus translated
 * zerovec), and re-normalized.
 */
static void SetPositionOrientFromTris(const vector3d v[6], SpaceStation::positionOrient_t &outOrient)
{
	outOrient.pos = v[0];
	outOrient.xaxis = (v[4] - v[1]).Normalized();
	outOrient.normal = (v[5] - v[1]).Normalized();
}

void SpaceStation::Init()
{
	m_adjacentCity = 0;
	SetModel(stationTypes[m_type].sbreModelName);
	const CollMeshSet *mset = GetModelCollMeshSet(GetSbreModel());
	int i=0;
	for (i=0; i<MAX_DOCKING_PORTS; i++) {
		port[i].exists = false;
		port_s2[i].exists = false;
	}
	vector3d v[6];
	for (i=0; i<MAX_DOCKING_PORTS; i++) {
		if (mset->GetTrisWithGeomflag(0x8000+i, 2, v) != 2) break;
		// 0x8000+i tri gives position, xaxis, yaxis of docking port surface
		port[i].exists = true;
		SetPositionOrientFromTris(v, port[i]);
		printf("%p port %d, %f,%f,%f, %f,%f,%f\n", this, i,
				port[i].xaxis.x,
				port[i].xaxis.y,
				port[i].xaxis.z,
				port[i].normal.x,
				port[i].normal.y,
				port[i].normal.z);
		if (mset->GetTrisWithGeomflag(0x8010+i, 2, v) != 2) continue;
		port_s2[i].exists = true;
		SetPositionOrientFromTris(v, port_s2[i]);
		printf("Got extended docking port info!\n");
	}
	m_numPorts = i;

	assert(m_numPorts > 0);
}

SpaceStation::~SpaceStation()
{
	for (std::vector<Mission*>::iterator i = m_bbmissions.begin();
			i != m_bbmissions.end(); ++i) {
		delete *i;
	}
	if (m_adjacentCity) delete m_adjacentCity;
}

void SpaceStation::ReplaceShipOnSale(int idx, const ShipFlavour *with)
{
	m_shipsOnSale[idx] = *with;
	onShipsForSaleChanged.emit();
}

void SpaceStation::UpdateShipyard()
{
	if (m_shipsOnSale.size() == 0) {
		// fill shipyard
		for (int i=Pi::rng.Int32(20); i; i--) {
			ShipFlavour s;
			ShipFlavour::MakeTrulyRandom(s);
			m_shipsOnSale.push_back(s);
		}
	} else if (Pi::rng.Int32(2)) {
		// add one
		ShipFlavour s;
		ShipFlavour::MakeTrulyRandom(s);
		m_shipsOnSale.push_back(s);
	} else {
		// remove one
		int pos = Pi::rng.Int32(m_shipsOnSale.size());
		m_shipsOnSale.erase(m_shipsOnSale.begin() + pos);
	}
	onShipsForSaleChanged.emit();
}

/* does not dealloc */
bool SpaceStation::BBRemoveMission(Mission *m)
{
	for (int i=m_bbmissions.size()-1; i>=0; i--) {
		if (m_bbmissions[i] == m) {
			m_bbmissions.erase(m_bbmissions.begin() + i);
			onBulletinBoardChanged.emit();
			return true;
		}
	}
	return false;
}

void SpaceStation::UpdateBB()
{
	if (m_bbmissions.size() == 0) {
		// fill bb
		for (int i=Pi::rng.Int32(20); i; i--) {
			try {
				Mission *m = Mission::GenerateRandom();
				m_bbmissions.push_back(m);
			} catch (CouldNotMakeMissionException) {

			}
		}
	} else if (Pi::rng.Int32(2)) {
		// add one
		try {
			Mission *m = Mission::GenerateRandom();
			m_bbmissions.push_back(m);
		} catch (CouldNotMakeMissionException) {

		}
	} else {
		// remove one
		int pos = Pi::rng.Int32(m_bbmissions.size());
		delete m_bbmissions[pos];
		m_bbmissions.erase(m_bbmissions.begin() + pos);
	}
	onBulletinBoardChanged.emit();
}

void SpaceStation::MoveDockingShips(const float timeStep)
{
	matrix4x4d rot;
	vector3d p2;
	for (int i=0; i<MAX_DOCKING_PORTS; i++) {
		shipDocking_t &dt = m_shipDocking[i];
		if (!dt.ship) continue;
		switch (dt.stage) {
		case 1:
			// open inner door
			dt.stagePos += 0.3*timeStep;
			if (dt.stagePos >= 1.0) {
				dt.stagePos = 0;
				dt.stage++;
			}
			break;
		case 2:
			// move into inner region
			GetRotMatrix(rot);
			p2 = GetPosition() + rot*port_s2[i].pos;
			dt.ship->SetPosition(dt.from + (p2-dt.from)*dt.stagePos);
			dt.stagePos += 0.1*timeStep;
			if (dt.stagePos >= 1.0) {
				dt.stagePos = 0;
				dt.stage++;
			}
			break;
		case 3:
			// close inner door & dock
			dt.stagePos += 0.3*timeStep;
			if (dt.stagePos >= 1.0) {
				dt.ship->SetDockedWith(this, i);
				dt.ship = 0;
				m_playerDockingTimeout = 0;
			}
			break;
		}
	}
}

void SpaceStation::TimeStepUpdate(const float timeStep)
{
	if (Pi::GetGameTime() > m_lastUpdatedShipyard) {
		UpdateBB();
		UpdateShipyard();
		// update again in an hour or two
		m_lastUpdatedShipyard = Pi::GetGameTime() + 3600.0 + 3600.0*Pi::rng.Double();
	}
	if (m_playerDockingTimeout > 0) {
		m_playerDockingTimeout -= timeStep;
		m_doorsOpen = MIN(m_doorsOpen+timeStep*0.2, 1.0);
		if (m_playerDockingTimeout < 0) {
			m_playerDockingTimeout = 0;
			Pi::onDockingClearanceExpired.emit(this);
		}
	} else {
		m_playerDockingTimeout = 0;
		m_doorsOpen = MAX(0.0, m_doorsOpen-timeStep*0.2);
	}
	MoveDockingShips(timeStep);
}

bool SpaceStation::IsGroundStation() const
{
	return (stationTypes[m_type].dockMethod ==
	        SpaceStationType::SURFACE);
}

void SpaceStation::OrientDockedShip(Ship *ship, int port) const
{
	const positionOrient_t *dport = &this->port[port];
	const int dockMethod = stationTypes[m_type].dockMethod;
	if (dockMethod == SpaceStationType::SURFACE) {
		matrix4x4d stationRot;
		GetRotMatrix(stationRot);
		vector3d port_z = vector3d::Cross(dport->xaxis, dport->normal);
		matrix4x4d rot = stationRot * matrix4x4d::MakeRotMatrix(dport->xaxis, dport->normal, port_z);
		vector3d pos = GetPosition() + stationRot*dport->pos;

		// position with wheels perfectly on ground :D
		Aabb aabb;
		ship->GetAabb(aabb);
		pos += stationRot*vector3d(0,-aabb.min.y,0);

		ship->SetPosition(pos);
		ship->SetRotMatrix(rot);
	}
}

void SpaceStation::OrientLaunchingShip(Ship *ship, int port) const
{
	const positionOrient_t *dport = &this->port[port];
	const int dockMethod = stationTypes[m_type].dockMethod;
	if (dockMethod == SpaceStationType::ORBITAL) {
		// position ship in middle of docking bay, pointing out of it
		// XXX need to do forced thrusting thingy...
		// XXX ang vel not zeroed for some reason...
		matrix4x4d stationRot;
		GetRotMatrix(stationRot);
		vector3d port_z = vector3d::Cross(dport->xaxis, dport->normal);
		printf("%f,%f,%f, %f,%f,%f, %f,%f,%f\n",
				dport->xaxis.x,
				dport->xaxis.y,
				dport->xaxis.z,
				dport->normal.x,
				dport->normal.y,
				dport->normal.z,
				port_z.x,
				port_z.y,
				port_z.z);

		matrix4x4d rot = stationRot * matrix4x4d::MakeRotMatrix(dport->xaxis, dport->normal, port_z);
		vector3d pos = GetPosition() + stationRot*dport->pos;
		ship->SetFrame(GetFrame());
		ship->SetPosition(pos);
		ship->SetRotMatrix(rot);
		ship->SetVelocity(GetFrame()->GetStasisVelocityAtPosition(pos));
		ship->SetAngVelocity(GetFrame()->GetAngVelocity());
		ship->SetForce(vector3d(0,0,0));
		ship->SetTorque(vector3d(0,0,0));
	}
	else if (dockMethod == SpaceStationType::SURFACE) {
		ship->Blastoff();
		Aabb aabb;
		ship->GetAabb(aabb);

	 	matrix4x4d stationRot;
		GetRotMatrix(stationRot);
		vector3d port_z = vector3d::Cross(dport->xaxis, dport->normal);
		matrix4x4d rot = stationRot * matrix4x4d::MakeRotMatrix(dport->xaxis, dport->normal, port_z);
		// position slightly (1m) off landing surface
		vector3d pos = GetPosition() + stationRot*(dport->pos +
				dport->normal - 
				dport->normal*aabb.min.y);
		ship->SetPosition(pos);
		ship->SetRotMatrix(rot);
	} else {
		assert(0);
	}
}

bool SpaceStation::GetDockingClearance(Ship *s)
{
	m_playerDockingTimeout = 300.0;
	return true;
}

/* MarketAgent shite */
void SpaceStation::Bought(Equip::Type t) {
	m_equipmentStock[(int)t]++;
}
void SpaceStation::Sold(Equip::Type t) {
	m_equipmentStock[(int)t]--;
}
bool SpaceStation::CanBuy(Equip::Type t) const {
	return true;
}
bool SpaceStation::CanSell(Equip::Type t) const {
	return m_equipmentStock[(int)t] > 0;
}
int SpaceStation::GetPrice(Equip::Type t) const {
	int mul = 100;
	SBody *sbody = GetFrame()->GetSBodyFor();
	if (sbody) {
		mul += sbody->tradeLevel[t];
	}
	return (mul * EquipType::types[t].basePrice) / 100;
}

bool SpaceStation::OnCollision(Body *b, Uint32 flags)
{
	if (flags & 0x10) {
		positionOrient_t *dport = &port[flags & 0xf];
		// hitting docking area of a station
		if (b->IsType(Object::SHIP)) {
			Ship *s = static_cast<Ship*>(b);
		
			double speed = s->GetVelocity().Length();
			
			// must be oriented sensibly and have wheels down
			if (IsGroundStation()) {
				matrix4x4d rot;
				s->GetRotMatrix(rot);
				matrix4x4d invRot = rot.InverseOf();
				
				matrix4x4d stationRot;
				GetRotMatrix(stationRot);
				vector3d dockingNormal = stationRot*dport->normal;

				// check player is sortof sensibly oriented for landing
				const double dot = vector3d::Dot(vector3d(invRot[1], invRot[5], invRot[9]), dockingNormal);
				if ((dot < 0.99) || (s->GetWheelState() != 1.0)) return true;
			}
			
			if ((speed < MAX_LANDING_SPEED) &&
			    (!s->GetDockedWith()) &&
			    m_playerDockingTimeout) {
				// if there is more docking port anim to do,
				// don't set docked yet
				if (port_s2[flags & 0xf].exists) {
					shipDocking_t &sd = m_shipDocking[flags&0xf];
					sd.ship = s;
					sd.stage = 1;
					sd.stagePos = 0;
					sd.from = s->GetPosition();
					s->DisableBodyOnly();
					s->SetFlightState(Ship::DOCKING);
					printf("Slide honky!\n");
				} else {
					m_playerDockingTimeout = 0;
					s->SetDockedWith(this, flags & 0xf);
				}
			}
		}
		if (!IsGroundStation()) return false;
		return true;
	} else {
		return true;
	}
}

void SpaceStation::NotifyDeath(const Body* const dyingBody)
{
	for (int i=0; i<MAX_DOCKING_PORTS; i++) {
		if (m_shipDocking[i].ship == dyingBody) {
			m_shipDocking[i].ship = 0;
		}
	}
}

void SpaceStation::Render(const Frame *camFrame)
{
	params.pAnim[ASRC_STATION_OPEN] = m_doorsOpen;
	params.pFlag[ASRC_STATION_OPEN] = 1;
	params.pAnim[ASRC_STATION_DOCK] = 0;
	params.pFlag[ASRC_STATION_DOCK] = 1;
	for (int i=0; i<MAX_DOCKING_PORTS; i++) {
		if (m_shipDocking[i].ship && (m_shipDocking[i].stage == 1)) {
			params.pAnim[ASRC_STATION_DOCK] = m_shipDocking[i].stagePos;
			break;
		}
		if (m_shipDocking[i].ship && (m_shipDocking[i].stage == 2)) {
			params.pAnim[ASRC_STATION_DOCK] = 1.0;
			break;
		}
		if (m_shipDocking[i].ship && (m_shipDocking[i].stage == 3)) {
			params.pAnim[ASRC_STATION_DOCK] = 1.0 - m_shipDocking[i].stagePos;
			break;
		}
	}
	RenderSbreModel(camFrame, &params);

	// find planet Body*
	Planet *planet;
	{
		Body *_planet = GetFrame()->m_astroBody;
		if ((!_planet) || !_planet->IsType(Object::PLANET)) {
			// orbital spaceport -- don't make city turds
		} else {
			planet = static_cast<Planet*>(_planet);
		
			if (!m_adjacentCity) {
				m_adjacentCity = new CityOnPlanet(planet, this, m_sbody->seed);
			}
			Shader::EnableVertexProgram(Shader::VPROG_SBRE);
			m_adjacentCity->Render(this, camFrame);
			Shader::DisableVertexProgram();
		}
	}
}
